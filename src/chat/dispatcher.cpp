#include "chat/dispatcher.hpp"

#include "chat/event/event.hpp"
#include "chat/event/filter/filter.hpp"
#include "chat/packet.hpp"
#include "chat/params.hpp"
#include "chat/transport/responder.hpp"
#include "database/database.hpp"
#include "platform/log.hpp"
#include "storage/sign.hpp"      // sha256_hex (th_files.content_hash)
#include "storage/storage.hpp"
#include "transcode/file.hpp"
#include "transcode/parser.hpp"
#include "llm/event.hpp"

#include <rapidjson/document.h>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace mirobody { namespace chat {

namespace {

// Emit an Error event to the responder (in-band failure reporting).
void emit_error(Responder& out, const std::string& msg) {
    out.send(ErrorEvent(msg));
}

// Add a string member to a JSON object (copying into the document's allocator).
void add_str(rapidjson::Value& obj, const char* key, const std::string& val,
             rapidjson::Document::AllocatorType& a) {
    obj.AddMember(rapidjson::StringRef(key),
                  rapidjson::Value(val.c_str(), static_cast<rapidjson::SizeType>(val.size()), a), a);
}

// Whether a file of `mime` is already plain text -- such uploads need no LLM
// extraction (the read path inlines their bytes directly).
bool is_text_mime(const std::string& mime) {
    if (mime.rfind("text/", 0) == 0) return true;
    return mime == "application/json" || mime == "application/xml" ||
           mime == "application/javascript" || mime == "image/svg+xml";
}

}   // namespace

Dispatcher::Dispatcher(Chat& chat, storage::Storage* storage, cache::Cache* cache,
                       file::Parser* parser, database::Database* db, memory::Memory* memory)
    : chat_(chat), storage_(storage), cache_(cache), parser_(parser), db_(db), memory_(memory) {}

//------------------------------------------------------------------------------
// Upload preprocess
//------------------------------------------------------------------------------

void Dispatcher::store_attachments(Packet& pkt, std::int64_t user_id, Responder& out) const {
    const std::vector<Attachment>& atts = pkt.attachments();
    if (atts.empty()) return;

    rapidjson::Value&                   params = pkt.params_mut();
    rapidjson::Document::AllocatorType& a      = pkt.allocator();

    // Find or create the "files" array in params (the reference list the command
    // params struct reads). Replace a non-array "files" rather than duplicate it.
    rapidjson::Value::MemberIterator fit = params.FindMember("files");
    if (fit != params.MemberEnd() && !fit->value.IsArray()) {
        params.RemoveMember("files");
        fit = params.MemberEnd();
    }
    if (fit == params.MemberEnd()) {
        params.AddMember("files", rapidjson::Value(rapidjson::kArrayType), a);
        fit = params.FindMember("files");
    }
    rapidjson::Value& files = fit->value;

    // Persist each attachment and append its reference. A file that fails to
    // store is logged and skipped rather than failing the whole turn. With no
    // object store configured (storage_ null) we keep name/type only.
    for (std::size_t i = 0; i < atts.size(); ++i) {
        const Attachment& att = atts[i];
        platform::log_debug("chat[2/store]: attachment '%s' (%lu bytes, mime=%s)",
                            att.filename.c_str(), (unsigned long)att.data.size(),
                            att.mime_type.c_str());
        rapidjson::Value f(rapidjson::kObjectType);
        add_str(f, "filename",  att.filename,  a);
        add_str(f, "mime_type", att.mime_type, a);

        if (storage_ != nullptr) {
            const std::string ctype = storage::resolve_content_type(att.filename, att.mime_type);

            // Store the bytes under their content-addressed per-user key.
            // Idempotent: a repeat upload of identical bytes lands on the
            // same object.
            storage::ObjectMeta meta;
            meta.filename = att.filename;
            std::string key;
            try {
                key = storage_->put_user_object(user_id, att.data, ctype, meta);
            } catch (const storage::StorageError& e) {
                platform::log_error("chat: failed to store upload '%s': %s", att.filename.c_str(), e.what());
                continue;   // skip this file; keep the turn going
            }
            // URLs are minted from the key, not stored by the put: a signed
            // read URL works on every backend (bucket-direct/local mount when
            // plaintext; an app-served decrypting endpoint when encryption is
            // on, since a client can't decrypt bucket bytes itself).
            const std::string url = storage_->signed_read_url(key);
            platform::log_debug("chat[2/store]: stored '%s' -> key=%s url=%s", att.filename.c_str(), key.c_str(), url.c_str());

            file::FileRef ref;
            ref.filename  = att.filename;
            ref.mime_type = ctype;
            ref.file_key  = key;
            ref.url       = url;
            const bool indexed = cache_ != nullptr && user_id > 0;

            // Notify the client the file is now in object storage.
            out.send(UploadEvent(att.filename, ctype, url, key));

#if !defined(MIROBODY_DATABASE_PG_LEGACY)
            // New-schema deployments index the upload in the `files` table (the
            // queryable index GET /api/files reads). Insert the row up front;
            // text_key is filled below once extraction finishes. Best-effort:
            // a failure is logged, never fails the turn.
            if (db_ != nullptr && user_id > 0) {
                try {
                    file::db_upsert_file(*db_, user_id, key, att.filename, ctype,
                                         static_cast<std::int64_t>(att.data.size()));
                } catch (const std::exception& e) {
                    platform::log_warn("chat: files index insert failed for '%s': %s",
                                       att.filename.c_str(), e.what());
                }
            }
#endif

            // Extract text from non-text uploads (image/PDF/...) once, now, and
            // persist it next to the original as <key>.trans, so reads fetch it
            // without re-parsing. The trans object itself is the dedup signal:
            // the key is content-addressed, so when it already exists these
            // exact bytes were extracted at an earlier upload -- reuse it
            // instead of re-running the LLM. Best-effort: a null parser, a
            // failed extraction, or a failed put just leaves text_key empty
            // (reads fall back to the object's bytes / URL). Only worth the
            // LLM call when an index will make it findable. The begin/done
            // events bracket the (slow) call so the client can show
            // extraction progress.
            //
            // extracted_text holds this turn's fresh extraction for the
            // th_files row below (PG_LEGACY); the reuse path leaves it empty
            // -- the conflict row from the original upload already carries
            // the text, and the upsert keeps it.
            std::string extracted_text;
            if (indexed && parser_ != nullptr && !is_text_mime(ctype)) {
                const std::string text_key = storage::get_trans_key(key);
                bool has_text = false;
                try {
                    has_text = storage_->object_exists(text_key);
                } catch (const storage::StorageError&) {
                    // Probe failed: fall through and re-extract -- costs an
                    // LLM call, never correctness.
                }
                if (has_text) {
                    ref.text_key = text_key;
                    platform::log_debug("chat[2/store]: reusing extracted text for '%s' (key=%s)",
                                        att.filename.c_str(), key.c_str());
                    // No Begin (nothing slow runs), but the client still
                    // learns this upload has a transcript: a re-upload reuses
                    // the text extracted the first time.
                    out.send(TranscriptEvent(TranscriptEvent::Phase::Done, att.filename, true));
                } else {
                    out.send(TranscriptEvent(TranscriptEvent::Phase::Begin, att.filename));
                    const std::string text = parser_->extract_text(att.data, ctype, att.filename);
                    const bool extracted = !text.empty();
                    if (extracted) {
                        extracted_text = text;
                        try {
                            storage_->put_object_encrypted(text_key, text, "text/plain; charset=utf-8");
                            ref.text_key = text_key;
                        } catch (const storage::StorageError& e) {
                            platform::log_error("chat: failed to store extracted text '%s': %s", text_key.c_str(), e.what());
                        }
                    }
                    out.send(TranscriptEvent(TranscriptEvent::Phase::Done, att.filename, extracted));
                }
            }

#if !defined(MIROBODY_DATABASE_PG_LEGACY)
            // Extraction has run: fill text_key on the row inserted above
            // (no-op when none was extracted / on a null db). Best-effort.
            if (db_ != nullptr && user_id > 0 && !ref.text_key.empty()) {
                try {
                    file::db_set_file_text_key(*db_, user_id, key, ref.text_key);
                } catch (const std::exception& e) {
                    platform::log_warn("chat: files index text_key update failed for '%s': %s",
                                       att.filename.c_str(), e.what());
                }
            }
#endif

            add_str(f, "url",      ref.url, a);
            add_str(f, "file_key", key, a);
            // Index the upload for this user so the MCP resources/list can
            // surface it (an atomic list append; for a dup this is just a
            // TTL refresh).
            if (indexed) {
                file::record(*cache_, user_id, ref);
            }

#if defined(MIROBODY_DATABASE_PG_LEGACY)
            // Legacy-schema deployments also keep th_files as the uploads
            // ledger the Python stack reads. Mirror its insert shape:
            // user_id as a decimal string (text column), file_name through
            // the schema's encrypt_content(), file_content left at its
            // '{}' default. The key is content-addressed, so a re-upload
            // upserts on file_key rather than duplicating the row.
            // Best-effort like the cache index: a failure is logged, never
            // fails the turn.
            if (db_ != nullptr && user_id > 0) {
                try {
                    // content_hash: SHA-256 hex of the file bytes, the same
                    // value the legacy stack computes for its dedup lookups.
                    // Re-set on conflict so rows from before the column was
                    // filled get backfilled by a re-upload.
                    //
                    // original_text / text_length: this turn's extraction
                    // (UTF-8 byte count). encrypt_content('') is NULL, so
                    // when nothing was extracted -- text mime, reuse path,
                    // failed extraction -- the conflict arm keeps whatever
                    // text the row already has instead of wiping it.
                    db_->execute(
                        "INSERT INTO th_files"
                        " (user_id, query_user_id, file_name, file_type, file_key, scene,"
                        "  created_source, content_hash, original_text, text_length)"
                        " VALUES (?, ?, encrypt_content(?), ?, ?, 'chat',"
                        "  'mirobody', ?, encrypt_content(?), ?)"
                        " ON CONFLICT (file_key) DO UPDATE SET"
                        "  file_name = EXCLUDED.file_name,"
                        "  file_type = EXCLUDED.file_type,"
                        "  content_hash = EXCLUDED.content_hash,"
                        "  original_text = COALESCE(EXCLUDED.original_text, th_files.original_text),"
                        "  text_length = CASE WHEN EXCLUDED.original_text IS NULL"
                        "                     THEN th_files.text_length ELSE EXCLUDED.text_length END,"
                        "  updated_at = now();",
                        {std::to_string(user_id), std::to_string(user_id), att.filename, ctype, key,
                         storage::sha256_hex(att.data), extracted_text,
                         static_cast<std::int64_t>(extracted_text.size())});
                } catch (const std::exception& e) {
                    platform::log_warn("chat: th_files record failed for '%s': %s",
                                       att.filename.c_str(), e.what());
                }
            }
#endif
        } else {
            add_str(f, "file_key", att.filename, a);   // no object store: metadata only
            // Still notify -- the file was received, just not durably stored
            // (no url/key beyond the name).
            out.send(UploadEvent(att.filename, att.mime_type, std::string(), att.filename));
        }

        files.PushBack(f, a);
    }
}

//------------------------------------------------------------------------------
// Dispatch
//------------------------------------------------------------------------------

void Dispatcher::dispatch(Packet& pkt, std::int64_t user_id, Responder& out) {
    // Preprocess: offload any uploaded bytes to object storage, recording
    // references in params.files for the command params to read. Emits
    // Upload / Transcript events into `out` so the client sees per-file
    // progress before the agent turn streams.
    store_attachments(pkt, user_id, out);

    // Translate each domain (llm) event to its chat event, run it through the
    // chat event-filter pipeline (e.g. render_chart -> ChartEvent), and send the
    // survivors to the Responder. Fresh pipeline per turn (filters are stateful).
    // Responder, pipeline, and `out` outlive this call (Chat invokes the handler
    // synchronously), so capturing by reference is safe.
    const EventPipeline pipeline = make_event_pipeline();
    llm::EventHandler sink = [&out, &pipeline](const llm::Event& le) -> bool {
        std::unique_ptr<Event> ev = event_from_llm(le);
        if (!ev) return true;
        return pipeline.feed(*ev, [&out](const Event& e) { return out.send(e); });
    };

    switch (pkt.code()) {
        case kOpChat: {
            ChatParams cp = ChatParams::parse(pkt, user_id);
            if (cp.agent.empty()) {
                emit_error(out, "empty agent name");
                break;
            }
            if (cp.request.messages.empty()) {   // empty iff no messages and no question
                emit_error(out, "empty question");
                break;
            }
            // Borrowed services for locally-executed tools (list_files /
            // read_file reach the per-user index + object store through these).
            cp.request.cache   = cache_;
            cp.request.storage = storage_;
            cp.request.memory  = memory_;
            cp.request.db      = db_;
            chat_.persist_history(cp.request);   // best-effort, before streaming
            chat_.response(cp.agent, cp.request, sink);
            break;
        }
        case kOpLive: {
            // The live path runs tools provider-side (MiroThinker), not through
            // the local executor, so it needs no service handles threaded in.
            LiveParams lp = LiveParams::parse(pkt);
            chat_.live_response(lp.request, sink);
            break;
        }
        default:
            emit_error(out, "unknown request code");
            break;
    }

    out.finish();
}

}}

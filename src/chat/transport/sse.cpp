#include "chat/transport/sse.hpp"

#include "chat/dispatcher.hpp"
#include "chat/event/event.hpp"
#include "chat/packet.hpp"
#include "chat/transport/responder.hpp"
#include "platform/log.hpp"
#include "server/auth.hpp"

#include <rapidjson/document.h>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <memory>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace mirobody { namespace chat {

namespace {

// Server-Sent Events outbound channel: each event becomes a `data: {json}\n\n`
// chunk on the HTTP StreamWriter; finish() delivers the terminal EndEvent chunk
// and closes the stream atomically (see the chunked-encoding note below).
class SseResponder : public Responder {
public:
    explicit SseResponder(std::shared_ptr<server::StreamWriter> writer)
        : writer_(std::move(writer)) {}

    bool send(const Event& e) override {
        writer_->write(e.to_sse());
        return !writer_->cancelled();
    }

    void finish() override {
        // finish(final_bytes): deliver the terminal "end" chunk and end-of-stream
        // atomically, so the router writes it with LWS_WRITE_HTTP_FINAL and lws
        // emits the chunked terminator. write()+finish() would race: the router
        // can drain the "end" chunk before finish() lands, then has no data left
        // to carry FINAL and the body is truncated (ERR_INCOMPLETE_CHUNKED_ENCODING).
        writer_->finish(EndEvent().to_sse());
    }

private:
    std::shared_ptr<server::StreamWriter> writer_;
};

// Add a string member to a JSON object iff the value is non-empty.
void add_str(rapidjson::Value& obj, const char* key, const std::string& val,
             rapidjson::Document::AllocatorType& a) {
    if (val.empty()) return;
    obj.AddMember(rapidjson::StringRef(key),
                  rapidjson::Value(val.c_str(), static_cast<rapidjson::SizeType>(val.size()), a), a);
}

// Set obj[key] = val, overwriting any existing member (unlike add_str, which
// would append a duplicate). Empty val is left untouched, so a header value
// overrides a body field only when actually present.
void set_str(rapidjson::Value& obj, const char* key, const std::string& val,
             rapidjson::Document::AllocatorType& a) {
    if (val.empty()) return;
    rapidjson::Value::MemberIterator it = obj.FindMember(key);
    if (it != obj.MemberEnd()) {
        it->value.SetString(val.c_str(), static_cast<rapidjson::SizeType>(val.size()), a);
    } else {
        obj.AddMember(rapidjson::StringRef(key),
                      rapidjson::Value(val.c_str(), static_cast<rapidjson::SizeType>(val.size()), a), a);
    }
}

// An SSE comment: a line starting with ':' carries no event, and every
// conformant reader drops it before the event layer -- which is what makes it
// the right keepalive here. It costs no protocol change and no client change:
// the web, Qt, Harmony and iOS readers all keep only `data:` lines, and Android
// goes through OkHttp's EventSource, which discards comments outright.
const char* const kKeepAlive = ": ping\n\n";

// Emit kKeepAlive on `writer` whenever the turn has sent nothing for
// `interval_ms`, until it ends. A turn is silent for long stretches by design --
// extracting text from an upload runs a vision model, a thinking model
// deliberates before its first token -- and nothing between here and the browser
// can tell a working stream from a dead one except by seeing bytes. This server
// deliberately disarms its own timeout for streams (server/router.cpp), so these
// bytes exist for the hops in between, whose idle timeouts are typically 60s.
//
// Runs on its own thread: the turn's thread is inside the dispatcher, blocked on
// exactly the calls this covers. The poll is coarse (200 ms) because it only has
// to notice the end of a turn promptly; writing is what the interval governs.
void pump_keepalives(std::shared_ptr<server::StreamWriter> writer, long long interval_ms) {
    for (;;) {
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
        if (writer->is_finished() || writer->cancelled()) return;
        if (writer->idle_ms() < interval_ms) continue;
        // Atomic against the turn ending underneath us: false means the terminal
        // chunk is already queued, and nothing may follow it.
        if (!writer->write_unless_finished(kKeepAlive)) return;
    }
}

}   // namespace

SseTransport::SseTransport(server::Router& router, Dispatcher& dispatcher, const jwt::Jwt& jwt,
                           int heartbeat_seconds)
    : router_(router), dispatcher_(dispatcher), jwt_(jwt),
      heartbeat_seconds_(heartbeat_seconds) {}

void SseTransport::start() {
    // POST /api/chat — stream a chat response as Server-Sent Events over a single
    // request. Authenticated by bearer JWT (header or ?token= query, per
    // require_auth). The body names an "agent" to run (chat::agent_registry).
    router_.post("/api/chat", server::require_auth(jwt_,
        [this](const server::Request& req, server::Response& res) {
            handle(req, res);
        }));
}

//------------------------------------------------------------------------------

bool SseTransport::parse_body(const server::Request& req, Packet& out, server::Response& res) {
    // The body is either JSON (the default) or an HTML form: a browser posts
    // application/x-www-form-urlencoded, and a file upload posts
    // multipart/form-data. Both encodings normalize to the same params object;
    // uploaded file bytes ride along as Packet attachments (stored later by the
    // dispatcher), never inside the JSON params. Request parses either form
    // shape (see server/request.hpp); we just read the fields and file parts.
    if (req.is_form_body()) {
        Packet pkt(kOpChat);
        rapidjson::Document::AllocatorType& a = pkt.allocator();
        rapidjson::Value& p = pkt.params_mut();

        add_str(p, "agent",      req.form_str("agent"),      a);
        add_str(p, "provider",   req.form_str("provider"),   a);
        add_str(p, "language",   req.form_str("language"),   a);
        add_str(p, "timezone",   req.form_str("timezone"),   a);
        add_str(p, "session_id", req.form_str("session_id"), a);
        add_str(p, "question",   req.form_str("question"),   a);

        // "messages" rides as a JSON-encoded array string in a form field.
        const std::string messages_json = req.form_str("messages");
        if (!messages_json.empty()) {
            rapidjson::Document md;
            if (!md.Parse(messages_json.c_str()).HasParseError() && md.IsArray()) {
                rapidjson::Value msgs;
                msgs.CopyFrom(md, a);
                p.AddMember(rapidjson::StringRef("messages"), msgs, a);
            }
        }

        // Uploaded files: attach the raw bytes; the dispatcher's preprocess
        // stores them and rewrites params.files. No storage logic here.
        std::vector<server::Request::FilePart>& files = req.form_files();
        platform::log_debug("chat[1/sse]: form parsed %lu file part(s)",
                            (unsigned long)files.size());
        for (std::size_t i = 0; i < files.size(); ++i) {
            server::Request::FilePart& fp = files[i];
            platform::log_debug("chat[1/sse]:   part[%lu] '%s' (%s, %lu bytes)",
                                (unsigned long)i, fp.filename.c_str(),
                                fp.content_type.c_str(), (unsigned long)fp.data.size());
            Attachment att;
            att.filename  = fp.filename;
            att.mime_type = fp.content_type;
            // The one place these bytes are copied into their own buffer -- and
            // it isn't a copy either: the part's storage is moved in, and every
            // hop from here on shares it (see mirobody::Blob).
            att.data      = Blob(std::move(fp.data));
            pkt.add_attachment(std::move(att));
        }

        out = std::move(pkt);
        return true;
    }

    // JSON body (application/json, or anything not recognized as a form): adopt
    // the parsed object as the packet's params.
    rapidjson::Document d;
    if (d.Parse(req.body.c_str()).HasParseError() || !d.IsObject()) {
        res.error(-1, "invalid request body", {}, 400);
        return false;
    }
    out = Packet(kOpChat, std::move(d));
    return true;
}

//------------------------------------------------------------------------------

void SseTransport::handle(const server::Request& req, server::Response& res) {
    Packet pkt;
    if (!parse_body(req, pkt, res)) return;

    // Stamp the transport-level turn context into the packet so the dispatcher
    // reads it from one place: the X-Session-Id / X-Timezone headers (or their
    // query/body fallbacks) override any matching body field. user_id is the
    // exception -- it is the JWT-verified identity, passed to dispatch separately
    // so a request body can never forge it.
    {
        rapidjson::Document::AllocatorType& a = pkt.allocator();
        rapidjson::Value& p = pkt.params_mut();
        set_str(p, "session_id", req.session_id, a);
        set_str(p, "timezone",   req.timezone,   a);
        set_str(p, "accept_language", req.accept_language, a);
    }
    const std::int64_t uid = req.user_id;

    res.header("Cache-Control", "no-cache");
    res.header("X-Accel-Buffering", "no");
    std::shared_ptr<server::StreamWriter> writer = res.begin_stream();

    // Run the turn on a worker thread: the dispatcher fires the responder on the
    // calling thread as upstream bytes arrive, so it must not block the lws
    // service loop. The Packet is move-only, so hand it to the thread via a
    // shared_ptr. `dispatcher` is stable (owned by the service, which outlives
    // the server).
    Dispatcher* dispatcher = &dispatcher_;
    std::shared_ptr<Packet> pktp = std::make_shared<Packet>(std::move(pkt));
    // Keep the connection warm through the turn's silent stretches. Started
    // before the turn and left to notice its end on its own: it holds the writer
    // by shared_ptr, so it is safe whichever thread finishes first.
    if (heartbeat_seconds_ > 0) {
        const long long interval_ms = static_cast<long long>(heartbeat_seconds_) * 1000;
        std::thread(pump_keepalives, writer, interval_ms).detach();
    }

    std::thread([writer, pktp, uid, dispatcher]() {
        writer->begin(200, "text/event-stream");
        SseResponder responder(writer);
        // Nothing runs above this frame: an exception that escapes a detached
        // thread is an uncaught exception, which takes the whole server down.
        // A turn that fails in a way it did not anticipate -- a decode that
        // could not allocate, a provider client throwing on malformed input --
        // must cost this request and no more, so it becomes an in-band error
        // event and a closed stream, exactly like a failure the turn did handle.
        try {
            dispatcher->dispatch(*pktp, uid, responder);
        } catch (const std::exception& e) {
            platform::log_error("chat: turn aborted by an unhandled exception: %s", e.what());
            responder.send(ErrorEvent(std::string("internal error: ") + e.what()));
            responder.finish();
        } catch (...) {
            platform::log_error("chat: turn aborted by an unknown exception");
            responder.send(ErrorEvent("internal error"));
            responder.finish();
        }
    }).detach();
}

}}

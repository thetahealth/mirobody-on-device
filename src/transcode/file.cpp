#include "transcode/file.hpp"
#include <optional>

#include "database/database.hpp"   // db-backed `files` index
#include "platform/clock.hpp"    // now_unix_ms
#include "platform/log.hpp"
#include "storage/sign.hpp"      // hmac_sha1, base64url_encode
#include "storage/storage.hpp"

#include <rapidjson/document.h>
#include <rapidjson/stringbuffer.h>
#include <rapidjson/writer.h>

#include <algorithm>
#include <array>
#include <ctime>

namespace mirobody { namespace file {

namespace {

// v3 keys (earlier layouts changed the value type / order at this name, so
// the version segment lets stale keys age out under their own TTL):
//
//   <base>:<id>       LIST, one serialized FileRef per row, newest upload
//                     first. The cache-layer truth: record() LPUSHes a row,
//                     which is atomic, so concurrent uploads can't lose each
//                     other's entries the way rewriting a shared value could.
//   <base>:json:<id>  STRING, the JSON array the read path serves, rendered
//                     from the list's rows in one atomic server-side step
//                     (see render_json), so it always reflects a current
//                     list state. Short TTL (kJsonTtl) as a backstop.
//   <base>:gen:<id>   STRING counter guarding the cold sidecar rebuild: see
//                     record() / list().
std::string list_key_for(std::int64_t user_id) {
    return "mirobody:user:files:" + std::to_string(user_id);
}

std::string json_key_for(std::int64_t user_id) {
    return "mirobody:user:files:json:" + std::to_string(user_id);
}

std::string gen_key_for(std::int64_t user_id) {
    return "mirobody:user:files:gen:" + std::to_string(user_id);
}

// How long the rendered JSON lives. The atomic render keeps it consistent
// with the list, so this is a backstop, not a race bound: it heals a writer
// that died between its LPUSH and its render (the rendering then misses
// that row until it expires). Re-rendering is one cache round-trip, so a
// short TTL costs little.
const std::chrono::hours kJsonTtl(1);

// rapidjson Value owning a copy of `s` (a bare const char* would dangle).
rapidjson::Value str_value(const std::string& s, rapidjson::Document::AllocatorType& a) {
    return rapidjson::Value(s.c_str(), static_cast<rapidjson::SizeType>(s.size()), a);
}

// Read a string member, or "" when absent / not a string.
std::string member_str(const rapidjson::Value& v, const char* key) {
    rapidjson::Value::ConstMemberIterator it = v.FindMember(key);
    if (it != v.MemberEnd() && it->value.IsString()) {
        return std::string(it->value.GetString(), it->value.GetStringLength());
    }
    return std::string();
}

// Read an integer member, or 0 when absent / not an integer.
std::int64_t member_int64(const rapidjson::Value& v, const char* key) {
    rapidjson::Value::ConstMemberIterator it = v.FindMember(key);
    if (it != v.MemberEnd() && it->value.IsInt64()) return it->value.GetInt64();
    return 0;
}

// One FileRef's fields from a JSON object (absent fields read as empty / 0).
FileRef ref_from_value(const rapidjson::Value& v) {
    FileRef f;
    f.filename    = member_str(v, "filename");
    f.mime_type   = member_str(v, "mime_type");
    f.file_key    = member_str(v, "file_key");
    f.url         = member_str(v, "url");
    f.text_key    = member_str(v, "text_key");
    f.uploaded_at = member_int64(v, "uploaded_at");
    return f;
}

// One FileRef's fields into a JSON object value.
rapidjson::Value ref_to_value(const FileRef& f, rapidjson::Document::AllocatorType& a) {
    rapidjson::Value o(rapidjson::kObjectType);
    o.AddMember("filename",    str_value(f.filename,  a), a);
    o.AddMember("mime_type",   str_value(f.mime_type, a), a);
    o.AddMember("file_key",    str_value(f.file_key,  a), a);
    o.AddMember("url",         str_value(f.url,       a), a);
    o.AddMember("text_key",    str_value(f.text_key,  a), a);
    o.AddMember("uploaded_at", f.uploaded_at, a);
    return o;
}

std::string to_json(const rapidjson::Value& v) {
    rapidjson::StringBuffer buf;
    rapidjson::Writer<rapidjson::StringBuffer> w(buf);
    v.Accept(w);
    return std::string(buf.GetString(), buf.GetSize());
}

// True when `f.file_key` is already in `out` -- the rebuild-vs-upload edge in
// list() can leave the same file twice in the cache list, so every decode
// path keeps only the first (newest) occurrence.
bool seen(const std::vector<FileRef>& out, const FileRef& f) {
    for (std::size_t i = 0; i < out.size(); ++i) {
        if (out[i].file_key == f.file_key) return true;
    }
    return false;
}

// Parse the rendered JSON (an array of FileRef objects, newest first);
// tolerant of malformed input (-> empty).
std::vector<FileRef> parse(const std::string& raw) {
    std::vector<FileRef> out;
    rapidjson::Document d;
    if (d.Parse(raw.c_str()).HasParseError() || !d.IsArray()) return out;
    out.reserve(d.Size());
    for (rapidjson::SizeType i = 0; i < d.Size(); ++i) {
        if (!d[i].IsObject()) continue;
        FileRef f = ref_from_value(d[i]);
        if (!f.file_key.empty() && !seen(out, f)) out.push_back(f);
    }
    return out;
}

// One FileRef as a standalone JSON object string -- one row of the cache
// list.
std::string serialize_one(const FileRef& f) {
    rapidjson::Document d;   // allocator host for the value
    return to_json(ref_to_value(f, d.GetAllocator()));
}

// Decode one list row. False when malformed or missing file_key.
bool parse_one(const std::string& raw, FileRef& out) {
    rapidjson::Document d;
    if (d.Parse(raw.c_str()).HasParseError() || !d.IsObject()) return false;
    out = ref_from_value(d);
    return !out.file_key.empty();
}

// Render the cache list into the JSON-array string key and return the
// rendering. Each row is already a serialized FileRef object, so the JSON
// is a join -- and set_join does the read-render-write as ONE atomic
// server-side step (a Lua script on Redis, the store mutex in-memory), so
// a rendering computed from an older list state can never overwrite one
// computed from a newer state: whichever render runs last saw every row
// pushed before it. nullopt when the list is empty / absent.
std::optional<std::string> render_json(cache::Cache& cache, std::int64_t user_id) {
    return cache.set_join(json_key_for(user_id), list_key_for(user_id),
                          "[", ",", "]", kJsonTtl);
}

// Decode a <file_key>.meta sidecar (written by Storage::put_user_object)
// into the display fields of a FileRef for `file_key`. The sidecar is JSON
// Lines, one record appended per upload of these bytes: the LAST record is
// the current view (filename, content type) and the FIRST record's stamp is
// the original upload time. Only those display fields come from the
// (client-influenced) document -- file_key, url, and text_key are derived
// server-side by the caller, so crafted sidecar content cannot point reads
// at other objects. Ownership needs no field here: every caller reaches a
// sidecar through the user's own hashed key prefix (the rebuild lists it;
// find() pins it via key_shape_ok). Malformed input is rejected.
bool meta_parse(const std::string& raw, const std::string& file_key, FileRef& out) {
    std::string first, last;
    std::size_t pos = 0;
    while (pos <= raw.size()) {
        const std::size_t eol = raw.find('\n', pos);
        std::string line = raw.substr(
            pos, eol == std::string::npos ? std::string::npos : eol - pos);
        const std::size_t b = line.find_first_not_of(" \t\r");
        if (b != std::string::npos) {
            line = line.substr(b, line.find_last_not_of(" \t\r") - b + 1);
            if (first.empty()) first = line;
            last = line;
        }
        if (eol == std::string::npos) break;
        pos = eol + 1;
    }
    if (last.empty()) return false;

    rapidjson::Document d;
    if (d.Parse(last.c_str()).HasParseError() || !d.IsObject()) return false;
    out = FileRef();
    out.file_key    = file_key;
    out.filename    = member_str(d, "filename");
    out.mime_type   = member_str(d, "content_type");
    out.uploaded_at = member_int64(d, "uploaded_at");

    // A re-upload appended records; keep the original upload's stamp.
    if (first != last) {
        rapidjson::Document f;
        if (!f.Parse(first.c_str()).HasParseError() && f.IsObject()) {
            const std::int64_t t = member_int64(f, "uploaded_at");
            if (t > 0) out.uploaded_at = t;
        }
    }
    return true;
}

// list()'s order: newest upload first; key as a tie-break so entries from
// before uploaded_at existed (stamp 0) sort stably.
bool ref_newer(const FileRef& a, const FileRef& b) {
    if (a.uploaded_at != b.uploaded_at) return a.uploaded_at > b.uploaded_at;
    return a.file_key < b.file_key;
}

// The caller's per-user object-key prefix: the first segment of every
// Storage::put_user_object key -- unpadded-base64url HMAC-SHA1 of the decimal
// user id, keyed by the storage's object-key seed (the bucket name, or a
// secret derived from FILE_ENCRYPTION_KEY when encryption is on -- see
// Storage::object_key_seed). Re-derived here (the upload path keeps the whole
// key derivation inline) for the index-rebuild scan and the ownership checks
// below; must mirror put_user_object exactly, so it goes through the same seed.
std::string user_segment(const storage::Storage& storage, std::int64_t user_id) {
    const std::array<unsigned char, 20> mac =
        storage::hmac_sha1(storage.object_key_seed(), std::to_string(user_id));
    return storage::base64url_encode(
        std::string(reinterpret_cast<const char*>(mac.data()), mac.size()));
}

// True when `key` has the exact server-generated shape for this caller:
// <user_seg>/<shard>/<27-char unpadded base64url digest>[.<suffix>] (the form
// storage::Storage::put_user_object produces; `user_seg` is the caller's
// user_segment above -- with the configured storage prefix in front for full
// keys; `shard` is a single base64url char equal to the digest's first char,
// the 64-way per-user folder fan-out; the suffix is the content-type-derived
// extension). find()'s sidecar fallback passes client-supplied keys to
// get_object, so this pins them to the caller's own namespace first -- another
// user's segment, path tricks ("..", embedded '/', empty segments), a mismatched
// shard, or any non-upload key shape is rejected before a storage round-trip.
bool key_shape_ok(const std::string& key, const std::string& user_seg) {
    auto is_b64url = [](char c) {
        return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
               (c >= '0' && c <= '9') || c == '-' || c == '_';
    };

    const std::string seg = user_seg + "/";
    if (key.compare(0, seg.size(), seg) != 0) return false;

    // <shard>/ : one base64url char then a separator.
    std::size_t p = seg.size();
    if (key.size() < p + 2) return false;
    const char shard = key[p];
    if (!is_b64url(shard) || key[p + 1] != '/') return false;
    p += 2;

    // 27-char base64url digest whose first char IS the shard.
    if (key.size() < p + 27 || key[p] != shard) return false;
    for (std::size_t i = p; i < p + 27; ++i) {
        if (!is_b64url(key[i])) return false;
    }
    p += 27;

    // ...followed by nothing, or a short lowercase-alnum extension.
    if (p == key.size()) return true;
    if (key[p] != '.') return false;
    ++p;
    if (p == key.size() || key.size() - p > 8) return false;
    for (; p < key.size(); ++p) {
        const char c = key[p];
        if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9'))) return false;
    }
    return true;
}

}   // namespace

//------------------------------------------------------------------------------

std::string iso8601_utc(std::int64_t secs) {
    const std::time_t t = static_cast<std::time_t>(secs);
    std::tm tm = std::tm();
#ifdef _WIN32
    gmtime_s(&tm, &t);
#else
    gmtime_r(&t, &tm);
#endif
    char buf[24];
    std::strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%SZ", &tm);
    return std::string(buf);
}

void record(cache::Cache& cache, std::int64_t user_id, const FileRef& f) {
    if (user_id <= 0 || f.file_key.empty()) return;
    const std::string list_key = list_key_for(user_id);

    // Dedup by key: content-addressed keys already collapse identical bytes,
    // so a repeat upload just refreshes the TTL -- the existing row (and its
    // first-upload uploaded_at stamp) stays as is.
    const std::vector<std::string> existing = cache.lrange(list_key, 0, -1);
    for (std::size_t i = 0; i < existing.size(); ++i) {
        FileRef e;
        if (parse_one(existing[i], e) && e.file_key == f.file_key) {
            cache.expire(list_key, kTtl);
            return;
        }
    }

    // First sight of this key: stamp the upload time unless the caller
    // supplied one.
    FileRef stamped = f;
    if (stamped.uploaded_at == 0) {
        stamped.uploaded_at = static_cast<std::int64_t>(std::time(nullptr));
    }

    // Bump the generation BEFORE touching the list, so a cold rebuild that
    // was already scanning storage when this upload landed sees it move and
    // keeps its hands off the keys (see list()). The TTL only keeps idle
    // users' counters from accumulating.
    cache.incr(gen_key_for(user_id));
    cache.expire(gen_key_for(user_id), kTtl);

    // 1. Push the row -- an atomic append, so concurrent uploads can't lose
    //    each other's rows -- then trim to the cap and refresh the TTL.
    cache.lpush(list_key, serialize_one(stamped));
    cache.ltrim(list_key, 0, static_cast<std::int64_t>(kMaxFiles) - 1);
    cache.expire(list_key, kTtl);

    // 2.+3. Read the whole list back and cache its rendering, as one atomic
    //    server-side step (see render_json) -- it reflects the list as it
    //    actually is after the push, rows from concurrent uploads included,
    //    and cannot be overwritten by a render of an older list state.
    render_json(cache, user_id);
}

std::vector<FileRef> list(cache::Cache& cache, storage::Storage* storage, std::int64_t user_id) {
    if (user_id <= 0) return std::vector<FileRef>();

    // Tier 1: the rendered JSON.
    const std::optional<std::string> raw = cache.get(json_key_for(user_id));
    if (raw.has_value()) return parse(*raw);

    // Tier 2: the rendering expired (kJsonTtl is short on purpose) but the
    // list is still warm -- atomically re-render from it, no storage
    // round-trips.
    const std::optional<std::string> rendered = render_json(cache, user_id);
    if (rendered.has_value()) return parse(*rendered);

    if (storage == nullptr) return std::vector<FileRef>();

    // Tier 3: cold (TTL expiry, eviction, restart) -- rebuild from the
    // sidecars. The generation before the scan: if an upload bumps it while
    // we read storage below, the rebuilt result is served but NOT cached --
    // it may already be missing that upload, and kTtl is a long time to be
    // stale. The next cold read just rebuilds again.
    const std::optional<std::string> gen = cache.get(gen_key_for(user_id));

    // The scan: one bounded list call over the caller's prefix plus a get
    // per sidecar. Each file is up to three objects (bytes, .meta, .trans),
    // so list a multiple of the entry cap. Only .meta keys are fetched: an
    // upload key can't take that shape (its suffix comes from the
    // content-type table), so every match is a genuine server-written
    // sidecar.
    std::vector<FileRef> files;
    try {
        const std::vector<std::string> keys = storage->list_user_objects(user_id, 4 * kMaxFiles);
        for (std::size_t i = 0; i < keys.size(); ++i) {
            const std::string& key = keys[i];
            if (!storage::is_meta_key(key)) continue;
            const std::string file_key =
                key.substr(0, key.size() - (sizeof(".meta") - 1));
            std::string side;
            try {
                side = storage->get_object_decrypted(key);
            } catch (const storage::StorageError&) {
                continue;   // raced away / transient / undecryptable: skip this entry
            }
            FileRef f;
            if (!meta_parse(side, file_key, f)) continue;
            // Derived, never read from the sidecar: a fresh signed read link,
            // and the extracted-text key when that object is in the same
            // listing (which is lexicographically ordered).
            f.url = storage->signed_read_url(file_key);
            const std::string text_key = storage::get_trans_key(file_key);
            if (std::binary_search(keys.begin(), keys.end(), text_key)) {
                f.text_key = text_key;
            }
            files.push_back(f);
        }
    } catch (const storage::StorageError& e) {
        platform::log_error("file: index rebuild for user %lld failed: %s",
                            (long long)user_id, e.what());
        return std::vector<FileRef>();
    }
    if (files.empty()) return files;

    // Newest first (key as a tie-break so entries from before uploaded_at
    // existed sort stably), trimmed to the cap.
    std::sort(files.begin(), files.end(), ref_newer);
    if (files.size() > kMaxFiles) files.resize(kMaxFiles);

    const std::optional<std::string> gen_now = cache.get(gen_key_for(user_id));
    const bool raced = gen.has_value() != gen_now.has_value() ||
                       (gen.has_value() && *gen != *gen_now);
    if (!raced) {
        std::vector<std::string> rows;
        rows.reserve(files.size());
        for (std::size_t i = 0; i < files.size(); ++i) {
            rows.push_back(serialize_one(files[i]));
        }
        // RPUSH, not DEL + push: a record() that slipped in between the
        // tier-2 render attempt and the gen read above already left its row
        // at the head, and appending keeps it (its scan-found duplicate is
        // dropped by parse()'s dedup). The atomic render then includes that
        // row too.
        cache.rpush(list_key_for(user_id), rows);
        cache.ltrim(list_key_for(user_id), 0, static_cast<std::int64_t>(kMaxFiles) - 1);
        cache.expire(list_key_for(user_id), kTtl);
        render_json(cache, user_id);
    }
    platform::log_debug("file: rebuilt index for user %lld from %lu sidecars%s",
                        (long long)user_id, (unsigned long)files.size(),
                        raced ? " (not cached: an upload raced the scan)" : "");
    return files;
}

bool find(cache::Cache& cache, storage::Storage* storage, std::int64_t user_id,
          const std::string& file_key, FileRef& out) {
    if (user_id <= 0 || file_key.empty()) return false;

    // Cache scan only (storage null): resolving one key shouldn't trigger a
    // full list rebuild -- the single-sidecar fallback below is cheaper.
    const std::vector<FileRef> files = list(cache, nullptr, user_id);
    for (std::size_t i = 0; i < files.size(); ++i) {
        if (files[i].file_key == file_key) {
            out = files[i];
            return true;
        }
    }

    // Not in the cached window (expired, trimmed, or recorded long ago): fall
    // back to the <file_key>.meta sidecar. key_shape_ok pins the
    // client-supplied key to the caller's own hashed prefix first -- that
    // prefix match IS the ownership check (the segment is a full HMAC of the
    // user id, so another user's key can't share it). Storage hands out full
    // keys (configured prefix in front), so that is the expected shape; the
    // bare form is also accepted for keys recorded before a prefix was
    // configured (reads prepend it back idempotently).
    if (storage == nullptr) return false;
    const std::string seg = user_segment(*storage, user_id);
    if (!key_shape_ok(file_key, storage->full_key(seg)) &&
        !key_shape_ok(file_key, seg)) {
        return false;
    }
    try {
        const std::string raw =
            storage->get_object_decrypted(storage::get_meta_key(file_key));
        if (!meta_parse(raw, file_key, out)) return false;
        out.url = storage->signed_read_url(file_key);
        // Optimistic: set even when no text was extracted -- the read path
        // already falls back to the object's bytes when the fetch 404s, and
        // probing here would cost the round-trip the reader pays anyway.
        out.text_key = storage::get_trans_key(file_key);
        return true;
    } catch (const storage::StorageError&) {
        return false;   // no sidecar (never stored) or storage unreachable
    }
}

//------------------------------------------------------------------------------
// Database-backed index (the `files` table)
//------------------------------------------------------------------------------

void db_upsert_file(database::Database& db, std::int64_t user_id,
                    const std::string& file_key, const std::string& filename,
                    const std::string& content_type, std::int64_t size_bytes) {
    if (user_id <= 0 || file_key.empty()) return;
    // One dialect for every new-schema backend. file_key is unique among active
    // rows (partial index WHERE deleted_at IS NULL), so a re-upload of identical
    // bytes updates the existing active row -- created_at (the upload time) is
    // kept and updated_at is stamped on that conflict; a fresh insert leaves
    // updated_at NULL (unix ms; the app maintains both columns).
    const std::int64_t now = platform::now_unix_ms();
    db.execute(
        "INSERT INTO files (user_id, file_key, filename, content_type, size_bytes, created_at) "
        "VALUES (?, ?, ?, ?, ?, ?) "
        "ON CONFLICT (file_key) WHERE deleted_at IS NULL DO UPDATE SET "
        "  filename = excluded.filename, "
        "  content_type = excluded.content_type, "
        "  size_bytes = excluded.size_bytes, "
        "  updated_at = ?;",
        {user_id, file_key, filename, content_type, size_bytes, now, now});
}

void db_set_file_text_key(database::Database& db, std::int64_t user_id,
                          const std::string& file_key, const std::string& text_key) {
    if (user_id <= 0 || file_key.empty() || text_key.empty()) return;
    db.execute(
        "UPDATE files SET text_key = ?, updated_at = ? "
        "WHERE file_key = ? AND user_id = ? AND deleted_at IS NULL;",
        {text_key, platform::now_unix_ms(), file_key, user_id});
}

std::vector<FileRow> db_list_files(database::Database& db, std::int64_t user_id,
                                   const std::string& sort, bool descending,
                                   int limit, int offset) {
    std::vector<FileRow> out;
    if (user_id <= 0) return out;

    // ORDER BY can't be a bound parameter; map `sort` to a fixed column so no
    // caller input reaches the SQL text. `id` is the stable tie-break.
    const char* col = (sort == "filename") ? "filename" : "created_at";
    const char* dir = descending ? "DESC" : "ASC";
    const std::string sql =
        "SELECT id, filename, content_type, file_key, text_key, summary, size_bytes, created_at "
        "FROM files WHERE user_id = ? AND deleted_at IS NULL "
        "ORDER BY " + std::string(col) + " " + dir + ", id DESC LIMIT ? OFFSET ?;";

    database::Result r = db.execute(sql, {user_id, limit, offset});
    for (std::size_t i = 0; i < r.rows.size(); ++i) {
        const std::vector<database::Value>& row = r.rows[i];
        if (row.size() < 8) continue;
        FileRow f;
        f.id         = row[0].is_null() ? 0 : row[0].as_int();
        f.filename   = row[1].is_null() ? std::string() : row[1].as_text();
        f.mime_type  = row[2].is_null() ? std::string() : row[2].as_text();
        f.file_key   = row[3].is_null() ? std::string() : row[3].as_text();
        f.text_key   = row[4].is_null() ? std::string() : row[4].as_text();
        f.summary    = row[5].is_null() ? std::string() : row[5].as_text();
        f.size_bytes = row[6].is_null() ? 0 : row[6].as_int();
        f.created_at = row[7].is_null() ? 0 : row[7].as_int();
        out.push_back(f);
    }
    return out;
}

std::int64_t db_count_files(database::Database& db, std::int64_t user_id) {
    if (user_id <= 0) return 0;
    database::Result r = db.execute(
        "SELECT COUNT(*) FROM files WHERE user_id = ? AND deleted_at IS NULL;", {user_id});
    if (r.rows.empty() || r.rows[0].empty() || r.rows[0][0].is_null()) return 0;
    return r.rows[0][0].as_int();
}

}}

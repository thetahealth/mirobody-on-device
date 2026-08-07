#include "mcp/service.hpp"

#include "database/database.hpp"   // user_mcp_access_logs insert
#include "database/enums.hpp"      // McpKind
#include "mcp/resource.hpp"
#include "mcp/tool.hpp"
#include "platform/clock.hpp"      // now_unix_ms
#include "platform/log.hpp"
#include "storage/sign.hpp"        // base64_encode
#include "storage/storage.hpp"     // Storage, StorageError
#include "transcode/file.hpp"
#include "transcode/parser.hpp"   // cap_text (the shared bound on file text)

#include <openssl/rand.h>

#include <rapidjson/document.h>
#include <rapidjson/stringbuffer.h>
#include <rapidjson/writer.h>

#include <chrono>
#include <cstdlib>
#include <cstring>
#include <string>
#include <utility>
#include <vector>

namespace mirobody { namespace mcp {

namespace {

// JSON-RPC 2.0 error codes (subset), matching the Python reference's mcp
// service. Application errors live in the -32000..-32099 band.
const int CODE_PARSE_ERROR      = -32700;
const int CODE_INVALID_REQUEST  = -32600;
const int CODE_METHOD_NOT_FOUND = -32601;
const int CODE_INVALID_PARAMS   = -32602;

// Cache key prefix for the personal-MCP secret mapping, stored both ways:
//   <prefix><secret>  -> user_id   (lookup on an incoming /mcp/{secret})
//   <prefix><user_id> -> secret    (reuse on a repeat /personal/mcp)
// Mirrors the Python reference's `mirobody:mcp:url:` Redis prefix.
const char* const kSecretKeyPrefix = "mirobody:mcp:url:";

// Personal-MCP secret lifetime: one year, matching the Python reference.
const std::chrono::hours kSecretTtl(24 * 365);

//------------------------------------------------------------------------------

// Records one user_mcp_access_logs row when a dispatch scope exits, stamping
// the handler latency and whether it succeeded. `success` defaults to false, so
// any early return -- auth failure, not-found, a thrown handler -- is logged as
// a failed access; the success path calls succeeded() just before responding.
// `user_id` is handle()'s up-front-resolved req.user_id (final before any method
// branch), so even a non-auth tool invoked by a signed-in caller is attributed.
// The write no-ops on a null store or an anonymous caller, so the guard is safe
// to arm always. (Compiled out on the legacy backend, which lacks the table.)
class McpAccessScope {
public:
    McpAccessScope(database::Database* db, std::int64_t user_id,
                   database::McpKind kind, std::string name)
        : db_(db), user_id_(user_id), kind_(kind), name_(std::move(name)),
          start_(std::chrono::steady_clock::now()) {}

    McpAccessScope(const McpAccessScope&)            = delete;
    McpAccessScope& operator=(const McpAccessScope&) = delete;

    void succeeded() { success_ = true; }

    ~McpAccessScope() { log_mcp_access(); }

private:
    // Record this access (latency since construction + outcome) to
    // user_mcp_access_logs on scope exit. A no-op without a store or for an
    // anonymous caller (the table's user_id is NOT NULL with an FK to users).
    // Best-effort: a failed write is logged and swallowed.
    void log_mcp_access() {
#if defined(MIROBODY_DATABASE_PG_LEGACY)
        // The legacy schema has no user_mcp_access_logs table.
#else
        if (db_ == nullptr || user_id_ <= 0) return;
        const std::int64_t ms = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - start_).count();
        // `name` is VARCHAR(128) on pg/mysql; cap so a long resource URI doesn't
        // overflow it (pg rejects, rather than truncates, an over-long value).
        std::string capped = name_;
        if (capped.size() > 128) capped.resize(128);
        try {
            db_->execute(
                "INSERT INTO user_mcp_access_logs (user_id, kind, name, is_success, duration_ms, created_at) "
                "VALUES (?, ?, ?, ?, ?, ?);",
                {user_id_, static_cast<int>(kind_), capped, success_ ? 1 : 0,
                 ms, platform::now_unix_ms()});
        } catch (const std::exception& e) {
            platform::log_warn("audit: mcp access log failed: %s", e.what());
        }
#endif
    }

    database::Database*                   db_;
    std::int64_t                          user_id_;
    database::McpKind                     kind_;
    std::string                           name_;
    std::chrono::steady_clock::time_point start_;
    bool                                  success_ = false;
};

//------------------------------------------------------------------------------

// URL-safe base64 (RFC 4648 sec.5), no padding. Used to render random secret
// bytes as a compact URL path segment, the analog of secrets.token_urlsafe.
std::string b64url(const unsigned char* data, std::size_t n) {
    static const char alpha[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";
    std::string out;
    out.reserve((n + 2) / 3 * 4);
    std::size_t i = 0;
    for (; i + 3 <= n; i += 3) {
        unsigned v = (static_cast<unsigned>(data[i]) << 16) |
                     (static_cast<unsigned>(data[i + 1]) << 8) |
                      static_cast<unsigned>(data[i + 2]);
        out += alpha[(v >> 18) & 63];
        out += alpha[(v >> 12) & 63];
        out += alpha[(v >> 6) & 63];
        out += alpha[v & 63];
    }
    const std::size_t rem = n - i;
    if (rem == 1) {
        unsigned v = static_cast<unsigned>(data[i]) << 16;
        out += alpha[(v >> 18) & 63];
        out += alpha[(v >> 12) & 63];
    } else if (rem == 2) {
        unsigned v = (static_cast<unsigned>(data[i]) << 16) |
                     (static_cast<unsigned>(data[i + 1]) << 8);
        out += alpha[(v >> 18) & 63];
        out += alpha[(v >> 12) & 63];
        out += alpha[(v >> 6) & 63];
    }
    return out;
}

// A fresh personal-MCP secret: 48 random bytes -> 64 url-safe base64 chars.
std::string make_secret() {
    unsigned char buf[48];
    if (RAND_bytes(buf, sizeof(buf)) != 1) {
        for (std::size_t i = 0; i < sizeof(buf); ++i) {
            buf[i] = static_cast<unsigned char>(std::rand() & 0xFF);
        }
    }
    return b64url(buf, sizeof(buf));
}

// "http" for localhost (dev), "https" otherwise -- matching the Python
// reference's hostname check. `host` is the raw Host header (may carry a port).
const char* url_scheme(const std::string& host) {
    if (host.rfind("localhost", 0) == 0 || host.rfind("127.0.0.1", 0) == 0) {
        return "http";
    }
    return "https";
}

//------------------------------------------------------------------------------

std::string serialize(const rapidjson::Value& v) {
    rapidjson::StringBuffer buf;
    rapidjson::Writer<rapidjson::StringBuffer> w(buf);
    v.Accept(w);
    return std::string(buf.GetString(), buf.GetSize());
}

// JSON-encode a std::string as a quoted JSON string literal (escaping). Used to
// splice a plain message or a pre-serialized JSON blob into a string field.
std::string json_quote(const std::string& s) {
    rapidjson::StringBuffer buf;
    rapidjson::Writer<rapidjson::StringBuffer> w(buf);
    w.String(s.c_str(), static_cast<rapidjson::SizeType>(s.size()));
    return std::string(buf.GetString(), buf.GetSize());
}

// The JSON-RPC `id` echoed back verbatim. Absent -> JSON null, per the spec
// note in the Python reference that the id field should always be present.
std::string id_json(const rapidjson::Document& req) {
    rapidjson::Value::ConstMemberIterator it = req.FindMember("id");
    if (it == req.MemberEnd()) return "null";
    return serialize(it->value);
}

//------------------------------------------------------------------------------
// JSON-RPC envelope writers

void send_result(server::Response& res, const std::string& id, const std::string& result_json) {
    res.json("{\"jsonrpc\":\"2.0\",\"id\":" + id + ",\"result\":" + result_json + "}");
}

void send_error(server::Response& res, const std::string& id, int code, const std::string& message) {
    res.json("{\"jsonrpc\":\"2.0\",\"id\":" + id +
             ",\"error\":{\"code\":" + std::to_string(code) +
             ",\"message\":" + json_quote(message) + "}}");
}

//------------------------------------------------------------------------------
// tools/call result envelope

// Wrap a tool Result into the MCP content/isError/structuredContent shape, the
// analog of the tail of Python service.py's tools/call branch. `r.json` is a
// serialized JSON document on success (echoed as the text payload and, when it
// is an object or array, also as structuredContent) or a plain error message.
std::string build_call_result(const Result& r) {
    rapidjson::Document doc;
    doc.SetObject();
    rapidjson::Document::AllocatorType& a = doc.GetAllocator();

    rapidjson::Value item(rapidjson::kObjectType);
    item.AddMember("type", "text", a);
    item.AddMember("text",
                   rapidjson::Value(r.json.c_str(),
                                    static_cast<rapidjson::SizeType>(r.json.size()), a),
                   a);

    rapidjson::Value content(rapidjson::kArrayType);
    content.PushBack(item, a);

    doc.AddMember("content", content, a);
    doc.AddMember("isError", !r.success, a);

    if (r.success) {
        rapidjson::Document data;
        if (!data.Parse(r.json.c_str()).HasParseError() &&
            (data.IsObject() || data.IsArray())) {
            doc.AddMember("structuredContent", rapidjson::Value(data, a), a);
        }
    }

    return serialize(doc);
}

//------------------------------------------------------------------------------
// resources/read result envelope

// Wrap a ResourceResult into the MCP `contents` array shape:
//   { "contents": [ { "uri", "mimeType", "text" | "blob" } ] }
// The per-read mime type wins over `fallback_mime`; an empty mime on both is
// simply omitted. Binary payloads (is_blob) are emitted as a base64 "blob"
// field, text as "text".
std::string build_read_result(const std::string& uri, const std::string& fallback_mime,
                              const ResourceResult& rr) {
    rapidjson::Document doc;
    doc.SetObject();
    rapidjson::Document::AllocatorType& a = doc.GetAllocator();

    rapidjson::Value item(rapidjson::kObjectType);
    item.AddMember("uri",
                   rapidjson::Value(uri.c_str(),
                                    static_cast<rapidjson::SizeType>(uri.size()), a),
                   a);

    const std::string& mime = !rr.mime_type.empty() ? rr.mime_type : fallback_mime;
    if (!mime.empty()) {
        item.AddMember("mimeType",
                       rapidjson::Value(mime.c_str(),
                                        static_cast<rapidjson::SizeType>(mime.size()), a),
                       a);
    }

    const char* field = rr.is_blob ? "blob" : "text";
    item.AddMember(rapidjson::StringRef(field),
                   rapidjson::Value(rr.content.c_str(),
                                    static_cast<rapidjson::SizeType>(rr.content.size()), a),
                   a);

    rapidjson::Value contents(rapidjson::kArrayType);
    contents.PushBack(item, a);

    doc.AddMember("contents", contents, a);
    return serialize(doc);
}

//------------------------------------------------------------------------------
// User-file resources (the caller's uploads, indexed in the cache by file/)

// The URI scheme that distinguishes an uploaded-file resource from a static
// registry one. The remainder after the prefix is the object-storage key.
const char* const kFilePrefix = "file://";

// Whether a payload of `mime` should ride in the MCP "text" field (UTF-8) vs a
// base64 "blob". Text-ish types inline as text so a client can read them
// directly; everything else (images, pdf, binary) is base64-encoded.
bool is_text_mime(const std::string& mime) {
    if (mime.rfind("text/", 0) == 0) return true;
    return mime == "application/json" || mime == "application/xml" ||
           mime == "application/javascript" || mime == "image/svg+xml";
}

// Build a resources/list descriptor for one uploaded file:
//   { "uri":"file://<key>", "name":<filename>, "description":..., "mimeType":... }
rapidjson::Value build_file_descriptor(const file::FileRef& f,
                                       rapidjson::Document::AllocatorType& a) {
    using rapidjson::Value;
    const std::string uri = kFilePrefix + f.file_key;
    const std::string name = f.filename.empty() ? f.file_key : f.filename;

    Value res(rapidjson::kObjectType);
    res.AddMember("uri",  Value(uri.c_str(),  static_cast<rapidjson::SizeType>(uri.size()),  a), a);
    res.AddMember("name", Value(name.c_str(), static_cast<rapidjson::SizeType>(name.size()), a), a);
    // Advertise the transcript: resources/read prefers the text extracted at
    // upload, so a client (or model) should not skip an image/PDF as
    // unreadable -- reading it yields text. mimeType stays the file's own
    // type; the read result carries its actual text/plain mimeType itself.
    if (!f.text_key.empty()) {
        res.AddMember("description",
                      "A file you uploaded. Text was extracted from it; "
                      "reading this resource returns that text.", a);
    } else {
        res.AddMember("description", "A file you uploaded.", a);
    }
    if (!f.mime_type.empty()) {
        res.AddMember("mimeType",
                      Value(f.mime_type.c_str(),
                            static_cast<rapidjson::SizeType>(f.mime_type.size()), a),
                      a);
    }
    if (f.uploaded_at > 0) {   // 0: entry predates the field
        // MCP resource annotations carry timestamps as ISO 8601 lastModified;
        // uploads are immutable (content-addressed), so created == modified.
        const std::string ts = file::iso8601_utc(f.uploaded_at);
        Value ann(rapidjson::kObjectType);
        ann.AddMember("lastModified",
                      Value(ts.c_str(), static_cast<rapidjson::SizeType>(ts.size()), a), a);
        res.AddMember("annotations", ann, a);
    }
    return res;
}

}   // namespace

//------------------------------------------------------------------------------

McpService::McpService(server::Router& router, const Config& cfg,
                       const jwt::Jwt& jwt, cache::Cache& cache,
                       storage::Storage* storage, memory::Memory* memory,
                       database::Database* db)
    : cfg_(cfg)
    , jwt_(jwt)
    , cache_(cache)
    , storage_(storage)
    , memory_(memory)
    , db_(db)
    , protocol_version_("2025-06-18")
    , name_("Theta MCP Server")
    , version_("1.0.0") {
    (void)cfg_;   // reserved for upstream/server identity wiring
    register_routes(router);
}

//------------------------------------------------------------------------------

void McpService::register_routes(server::Router& router) {
    // POST /mcp and POST /mcp/{secret} -- a single JSON-RPC 2.0 request/
    // response. The bare path authenticates via bearer JWT; the {secret} form
    // carries a personal-MCP secret that resolves to a user id. OPTIONS
    // preflight is answered centrally by the Router's CORS handling (it sees
    // POST registered on both paths). GET (the Python WebSocket TODO) is not
    // wired.
    router.post("/mcp", [this](const server::Request& req, server::Response& res) {
        handle(req, res);
    });
    router.post("/mcp/{secret}", [this](const server::Request& req, server::Response& res) {
        handle(req, res);
    });

    // POST /personal/mcp -- mint (or reuse) the caller's personal MCP URL.
    router.post("/personal/mcp", [this](const server::Request& req, server::Response& res) {
        generate_personal_mcp(req, res);
    });
}

//------------------------------------------------------------------------------

std::string McpService::user_for_secret(const std::string& secret) {
    if (secret.empty()) return std::string();
    mirobody::optional<std::string> v = cache_.get(kSecretKeyPrefix + secret);
    return v.has_value() ? *v : std::string();
}

//------------------------------------------------------------------------------

bool McpService::authenticate(const server::Request& req, server::Response& res,
                              const std::string& id, UserInfo& user) {
    // handle() already resolved the caller (path secret or bearer token) into
    // req.user_id; this is purely the gate for an auth-flagged method.
    user.user_id = req.user_id;
    if (user.authed()) {
        return true;
    }

    // Signal the OAuth 2.0 authorization server with an HTTP 401 +
    // WWW-Authenticate carrying the RFC 9728 protected-resource metadata URL.
    // This is the trigger that makes an MCP client discover the auth server
    // (served by oauth::OAuthService) and start the authorization-code + PKCE
    // flow. The JSON-RPC error body is included for clients that read it.
    const std::string meta = std::string(url_scheme(req.host)) + "://" +
                             req.host + "/.well-known/oauth-protected-resource";
    res.status(401);
    res.header("WWW-Authenticate", "Bearer resource_metadata=\"" + meta + "\"");
    send_error(res, id, -32001, "Authentication required");
    return false;
}

//------------------------------------------------------------------------------

void McpService::generate_personal_mcp(const server::Request& req, server::Response& res) {
    // Authenticate the caller via bearer JWT; the minted URL belongs to `sub`.
    jwt::Jwt::VerifyResult vr = jwt_.verify(req.authorization);
    if (!vr.ok()) {
        res.error(-1, vr.error.empty() ? "Invalid token" : vr.error);
        return;
    }
    // verify() decoded the opaque subject back to the row id we key on.
    if (vr.user_id <= 0) {
        res.error(-2, "Invalid user ID.");
        return;
    }
    const std::string user_id = std::to_string(vr.user_id);
    if (req.host.empty()) {
        res.error(-3, "Cannot determine server host.");
        return;
    }

    // Reuse the user's existing secret when one is on file, otherwise mint a
    // fresh one and store both directions of the mapping.
    std::string secret;
    mirobody::optional<std::string> existing = cache_.get(kSecretKeyPrefix + user_id);
    if (existing.has_value() && !existing->empty()) {
        secret = *existing;
    } else {
        secret = make_secret();
        cache_.set(kSecretKeyPrefix + secret, user_id, kSecretTtl);
        cache_.set(kSecretKeyPrefix + user_id, secret, kSecretTtl);
    }

    const std::string url =
        std::string(url_scheme(req.host)) + "://" + req.host + "/mcp/" + secret;

    res.ok("{\"url\":" + json_quote(url) + "}");
}

//------------------------------------------------------------------------------

void McpService::handle(const server::Request& req, server::Response& res) {
    rapidjson::Document doc;
    if (doc.Parse(req.body.c_str()).HasParseError() || !doc.IsObject()) {
        send_error(res, "null", CODE_PARSE_ERROR, "Invalid request body");
        return;
    }

    const std::string id = id_json(doc);

    // Resolve the caller's identity once, up front, into req.user_id -- the
    // same slot require_auth fills (req.user_id is mutable for exactly this).
    // Two sources: a personal-MCP secret in the URL (POST /mcp/{secret}) wins;
    // otherwise the bearer token. This only *populates* identity, it never
    // rejects -- discovery (initialize, tools/list, ...) must work for an
    // anonymous caller, so an absent/invalid token simply leaves req.user_id 0
    // and the per-method auth gate (authenticate) decides whether that matters.
    std::unordered_map<std::string, std::string>::const_iterator sit = req.path_params.find("secret");
    if (sit != req.path_params.end()) {
        // The mapped value is one we wrote as std::to_string(row_id), so a plain
        // parse is exact. An unknown/expired secret yields "" -> 0, and any
        // non-positive result leaves the caller unauthenticated (the downstream
        // req.user_id <= 0 / authed() gates fail closed).
        req.user_id = std::strtoll(user_for_secret(sit->second).c_str(), nullptr, 10);
    } else if (!req.authorization.empty()) {
        // verify() strips a leading "Bearer " itself and decodes the opaque
        // subject back to the row id; <= 0 means it didn't decode (forged /
        // wrong-salt / legacy), so we leave req.user_id 0.
        const jwt::Jwt::VerifyResult vr = jwt_.verify(req.authorization);
        if (vr.ok() && vr.user_id > 0) req.user_id = vr.user_id;
    }

    rapidjson::Value::ConstMemberIterator m = doc.FindMember("method");
    if (m == doc.MemberEnd() || !m->value.IsString() || m->value.GetStringLength() == 0) {
        send_error(res, id, CODE_INVALID_REQUEST, "Invalid MCP method");
        return;
    }
    const std::string method(m->value.GetString(), m->value.GetStringLength());

    //--------------------------------------------------------------------------
    // Discovery + lifecycle.

    if (method == "initialize") {
        const std::string result =
            "{\"protocolVersion\":" + json_quote(protocol_version_) +
            ",\"capabilities\":{\"prompts\":{},\"resources\":{},"
            "\"tools\":{\"listChanged\":false}}"
            ",\"serverInfo\":{\"name\":" + json_quote(name_) +
            ",\"version\":" + json_quote(version_) + "}}";
        send_result(res, id, result);
        return;
    }

    if (method == "notifications/initialized") {
        // A notification: acknowledge with an empty 200, no JSON-RPC body.
        res.status(200);
        res.text("");
        return;
    }

    if (method == "ping") {
        send_result(res, id, "{}");
        return;
    }

    if (method == "tools/list") {
        send_result(res, id, "{\"tools\":" + registry().tools_list_json() + "}");
        return;
    }

    if (method == "prompts/list") {
        send_result(res, id, "{\"prompts\":[]}");
        return;
    }

    if (method == "resources/list") {
        // Merge the static registry resources (visible to everyone) with the
        // caller's own uploaded files (only when we know who they are -- req.
        // user_id was resolved up front, from a path secret or bearer token).
        rapidjson::Document out;
        out.SetArray();
        rapidjson::Document::AllocatorType& a = out.GetAllocator();

        rapidjson::Document reg;
        if (!reg.Parse(resource_registry().resources_list_json().c_str()).HasParseError() &&
            reg.IsArray()) {
            for (rapidjson::SizeType i = 0; i < reg.Size(); ++i) {
                out.PushBack(rapidjson::Value(reg[i], a), a);   // deep-copy
            }
        }

        if (req.user_id > 0) {
            const std::vector<file::FileRef> files =
                file::list(cache_, storage_, req.user_id);
            for (std::size_t i = 0; i < files.size(); ++i) {
                out.PushBack(build_file_descriptor(files[i], a), a);
            }
        }

        send_result(res, id, "{\"resources\":" + serialize(out) + "}");
        return;
    }

    if (method == "resources/templates/list") {
        // No URI-templated resources yet; every registered resource has a fixed
        // uri and is surfaced by resources/list above.
        send_result(res, id, "{\"resourceTemplates\":[]}");
        return;
    }

    if (method == "resources/read") {
        rapidjson::Value::ConstMemberIterator pit = doc.FindMember("params");
        if (pit == doc.MemberEnd() || !pit->value.IsObject()) {
            send_error(res, id, CODE_INVALID_PARAMS, "Empty parameter");
            return;
        }
        const rapidjson::Value& params = pit->value;

        rapidjson::Value::ConstMemberIterator uit = params.FindMember("uri");
        if (uit == params.MemberEnd() || !uit->value.IsString() ||
            uit->value.GetStringLength() == 0) {
            send_error(res, id, CODE_INVALID_PARAMS, "Empty resource uri");
            return;
        }
        const std::string uri(uit->value.GetString(), uit->value.GetStringLength());

        // Log this read (success or failure) once, on scope exit. Identity for
        // the auth gate is resolved per sub-branch below into `user`.
        UserInfo user;
        McpAccessScope log(db_, req.user_id, database::McpKind::Resource, uri);

        // A file://<key> URI is one of the caller's uploads. These always
        // require identity, and the cache lookup doubles as the ownership check
        // (a user can only see keys in their own list), so guessing another
        // user's key yields "not found" rather than their bytes.
        if (uri.rfind(kFilePrefix, 0) == 0) {
            if (!authenticate(req, res, id, user)) return;

            const std::string file_key = uri.substr(std::strlen(kFilePrefix));
            file::FileRef ref;
            if (!file::find(cache_, storage_, user.user_id, file_key, ref)) {
                send_error(res, id, -32002, "Resource not found: " + uri);
                return;
            }
            if (storage_ == nullptr) {
                send_error(res, id, -32603, "Object storage is not configured");
                return;
            }
            // Prefer the text extracted at upload (image/PDF/...), stored next
            // to the original as <file_key>.trans; otherwise fall back to the
            // object's bytes (text inline, binary as a base64 blob). A missing
            // text object also falls back to the bytes.
            if (!ref.text_key.empty()) {
                try {
                    const std::string text = storage_->get_object_decrypted(ref.text_key);
                    log.succeeded();
                    send_result(res, id, build_read_result(
                        uri, "text/plain", ResourceResult::text(text, "text/plain")));
                    return;
                } catch (const storage::StorageError&) {
                }
            }
            std::string bytes;
            try {
                bytes = storage_->get_object_decrypted(file_key);
            } catch (const storage::StorageError& e) {
                send_error(res, id, -32603, e.what());
                return;
            }
            // A file whose bytes ARE its text never went through extraction, so
            // nothing has bounded it yet: apply the same cap a Parser applies to
            // what it extracts, instead of handing a multi-megabyte upload to a
            // model whole. (The .trans branch above is capped at write time.)
            const ResourceResult rr = is_text_mime(ref.mime_type)
                ? ResourceResult::text(file::cap_text(std::move(bytes)), ref.mime_type)
                : ResourceResult::blob(storage::base64_encode(bytes), ref.mime_type);
            log.succeeded();
            send_result(res, id, build_read_result(uri, ref.mime_type, rr));
            return;
        }

        const Resource* resource = resource_registry().find(uri);
        if (!resource) {
            // -32002 is the MCP-defined "resource not found" code.
            send_error(res, id, -32002, "Resource not found: " + uri);
            return;
        }

        // Resolve identity for auth-flagged resources, exactly as tools/call.
        if (resource->auth && !authenticate(req, res, id, user)) {
            return;
        }

        const ResourceResult rr = resource_registry().read(uri, user);
        if (rr.success) log.succeeded();
        if (!rr.success) {
            // -32603 is JSON-RPC's standard "internal error".
            send_error(res, id, -32603, rr.content);
            return;
        }
        send_result(res, id, build_read_result(uri, resource->mime_type, rr));
        return;
    }

    //--------------------------------------------------------------------------
    // tools/call.

    if (method == "tools/call") {
        rapidjson::Value::ConstMemberIterator pit = doc.FindMember("params");
        if (pit == doc.MemberEnd() || !pit->value.IsObject()) {
            send_error(res, id, CODE_INVALID_PARAMS, "Empty parameter");
            return;
        }
        const rapidjson::Value& params = pit->value;

        rapidjson::Value::ConstMemberIterator nit = params.FindMember("name");
        if (nit == params.MemberEnd() || !nit->value.IsString() || nit->value.GetStringLength() == 0) {
            send_error(res, id, CODE_INVALID_PARAMS, "Empty parameter name");
            return;
        }
        const std::string name(nit->value.GetString(), nit->value.GetStringLength());

        // Arm the access log up front so an unknown tool / auth failure is still
        // recorded against the (already-resolved) caller id.
        UserInfo user;
        McpAccessScope log(db_, req.user_id, database::McpKind::Tool, name);

        const Tool* tool = registry().find(name);
        if (!tool) {
            send_error(res, id, CODE_INVALID_PARAMS, "Unsupported tool: " + name);
            return;
        }

        // Resolve identity for auth-flagged tools; a 401 + WWW-Authenticate
        // challenge is written and we stop when authentication is required but
        // absent.
        if (tool->auth && !authenticate(req, res, id, user)) {
            return;
        }

        // Hand the params.arguments object (or a null sentinel when absent) to
        // the registry; Args reads from it tolerantly.
        static const rapidjson::Value kNull;   // kNullType
        rapidjson::Value::ConstMemberIterator ait = params.FindMember("arguments");
        const rapidjson::Value& arguments =
            (ait != params.MemberEnd()) ? ait->value : kNull;

        // Hand the handler this service's cache + object store so file-backed
        // tools (list_files / read_file) can reach the per-user index and bytes,
        // and the relational store so chat-history tools (summarize_conversation)
        // can reach it.
        const ToolContext ctx(&cache_, storage_, db_, memory_);
        const Result r = registry().call(name, arguments, user, ctx);
        if (r.success) log.succeeded();
        send_result(res, id, build_call_result(r));
        return;
    }

    //--------------------------------------------------------------------------

    send_error(res, id, CODE_METHOD_NOT_FOUND, "MCP method not found: " + method);
}

}}

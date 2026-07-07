#include "server/router.hpp"

#include "platform/log.hpp"
#include "storage/sign.hpp"      // hmac_sha256, hex_encode — presigned-URL verification
#include "storage/storage.hpp"   // file mount: get_object_decrypted, content-type resolution

#include <libwebsockets.h>

#include <zlib.h>              // gzip-compress large fixed-length response bodies

#include <rapidjson/stringbuffer.h>
#include <rapidjson/writer.h>

#include <sys/stat.h>

#include <algorithm>
#include <array>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <mutex>
#include <unordered_set>
#include <utility>

namespace mirobody { namespace server {

namespace {

constexpr size_t LWS_TX_BUFFER = 4096;

}

//------------------------------------------------------------------------------
// Per-connection session state
//------------------------------------------------------------------------------

struct PerHttpSession {
    std::string body_buf;
    std::string out_buf;
    size_t out_sent = 0;
    Response response;
    bool response_ready = false;

    // Whether the client advertised gzip in Accept-Encoding. Latched at dispatch
    // (the Request is gone by the time the writeable callback runs) and consulted
    // when framing a fixed-length body: a large text response is gzip-ed then.
    bool accept_gzip = false;

    // Chunked-streaming state, used only when response.is_streaming(). Headers
    // go out on the first writeable callback; each later callback drains
    // `stream_chunk` to the socket and, once it is empty, pulls the next chunk
    // from the producer. `producer_done` latches once the producer returns
    // false, after which a final zero-length chunk closes the body.
    bool headers_sent = false;
    std::string stream_chunk;
    size_t stream_off = 0;
    bool producer_done = false;
};

struct PerWsSession {
    WsHandlerHooks hooks;
    std::vector<std::pair<std::string, bool>> pending_writes;
    bool open = false;
};

//------------------------------------------------------------------------------
// Router internals
//------------------------------------------------------------------------------

struct Router::Impl {
    // HTTP routes keyed by "METHOD /path" (e.g. "GET /api/health"); method is a
    // fixed token with no spaces, so a single space delimiter is unambiguous.
    std::unordered_map<std::string, HttpHandler> http_routes;
    std::unordered_map<std::string, WsHandlerFactory> ws_routes;

    // Routes registered with `{name}` path segments (e.g. "/mcp/{secret}").
    // Kept apart from the exact-match http_routes map and consulted only on a
    // miss, so adding parameterized routes costs the common (exact) path
    // nothing. A literal segment must match verbatim; a "{name}" segment
    // matches any single segment and captures it into Request::path_params.
    struct ParamRoute {
        std::string              method;
        std::vector<std::string> segments;   // param segments store the bare name
        std::vector<bool>        is_param;
        HttpHandler              handler;
    };
    std::vector<ParamRoute> param_routes;

    std::mutex ws_write_mutex;
    std::unordered_map<lws*, std::vector<std::pair<std::string, bool>>> external_writes;

    // HTTP connections currently serving an async (StreamWriter) response. Only
    // touched on the service thread -- at dispatch, on close, and from the
    // EVENT_WAIT_CANCELLED wakeup -- so it needs no lock.
    std::unordered_set<lws*> active_streams;

    std::vector<lws_protocols> protocols;

    // Headers added to every response (the HTTP_HEADERS config map). Empty =>
    // none, and OPTIONS preflight is not answered centrally.
    std::vector<std::pair<std::string, std::string>> default_headers;

    // Directories served as static content for unmatched GETs (HTTP_ROOT). Each
    // entry is {mount-path, directory}; a request under a mount-path is served
    // from its directory (mount-path stripped first), so several frontends sit
    // at distinct URL prefixes. Mount-paths are normalized (leading slash, no
    // trailing slash; "/" is the root) and the list is ordered
    // most-specific-prefix-first, so a deeper mount wins over the "/" catch-all.
    // Empty disables static serving.
    std::vector<std::pair<std::string, std::string>> http_roots;

    // LocalStorage HTTP mount (LOCAL_STORAGE_*). When storage_mount and
    // storage_dir are set, GETs whose path is under storage_mount are served
    // from storage_dir -- which, unlike http_root, may live OUTSIDE the web root
    // so user uploads aren't exposed as ordinary static assets. When
    // storage_secret is also set, each request must carry a valid presigned
    // "?expires=&sig=" HMAC (minted by LocalStorage::presigned_url) or it is
    // rejected. See set_storage_mount / serve_storage_file.
    std::string storage_mount;    // URL path prefix: leading '/', no trailing '/' (e.g. "/files")
    std::string storage_dir;      // disk root the objects live under (LOCAL_STORAGE_DIR)
    std::string storage_secret;   // HMAC key; empty => serve without signature checks

    // Decrypting file mount (FILE_ENCRYPTION_KEY): serves encrypted uploads
    // through this server, fetched + decrypted via `file_storage`. Active when
    // file_mount and file_storage are both set; supersedes the disk mount above
    // (which would serve ciphertext). file_storage is borrowed (owned by Server).
    std::string       file_mount;
    storage::Storage* file_storage = nullptr;
    std::string       file_secret;   // HMAC key for the "?expires=&sig=" check

    // Prefix prepended to every registered route path and stripped from request
    // paths before static-file resolution (HTTP_URI_PREFIX). Normalized to a
    // leading slash and no trailing slash (e.g. "/mirobody"); empty disables it.
    std::string uri_prefix;

    Router* self = nullptr;

    static std::string route_key(const std::string& method, const std::string& path) {
        return method + " " + path;
    }

    // A route path with the configured uri_prefix prepended. `path` always
    // starts with '/', so plain concatenation yields "/mirobody" + "/x".
    std::string with_prefix(const std::string& path) const {
        if (uri_prefix.empty()) return path;
        return uri_prefix + path;
    }

    // Remove the uri_prefix from a request path for static-file resolution, so
    // files under http_root are addressed without it ("/mirobody/app.js" ->
    // "/app.js", "/mirobody" -> "/"). When a prefix is set, a path NOT under it
    // returns "" so the caller serves nothing and 404s -- the app exists only
    // under the mount, never at the bare root (which would otherwise load the
    // SPA at the wrong base and make its prefix-less API calls 404).
    std::string strip_prefix(const std::string& path) const {
        if (uri_prefix.empty()) return path;
        if (path == uri_prefix) return "/";
        if (path.size() > uri_prefix.size() &&
            path.compare(0, uri_prefix.size(), uri_prefix) == 0 &&
            path[uri_prefix.size()] == '/') {
            return path.substr(uri_prefix.size());
        }
        return std::string();
    }

    // If `stripped` (a request path with the uri_prefix already removed) is the
    // bare root of a non-root static mount ("/dashboard" for a mount at
    // "/dashboard"), return the location to redirect it to ("/dashboard/", with
    // the uri_prefix added back). Empty => no redirect. Like the bare-uri_prefix
    // redirect, the trailing slash makes the document base the mount so the
    // frontend's relative asset URLs resolve under it rather than its parent.
    std::string bare_mount_redirect(const std::string& stripped) const {
        for (const auto& mount : http_roots) {
            if (mount.first != "/" && stripped == mount.first) {
                return with_prefix(mount.first) + "/";
            }
        }
        return std::string();
    }

    // Split a request path into its '/'-delimited segments, dropping the
    // leading empty segment from the root slash (so "/mcp/abc" -> {"mcp","abc"}
    // and "/mcp" -> {"mcp"}). A trailing slash yields a trailing empty segment,
    // which simply fails to match a pattern that has none.
    static std::vector<std::string> split_segments(const std::string& path) {
        std::vector<std::string> out;
        std::size_t i = (!path.empty() && path[0] == '/') ? 1 : 0;
        while (i <= path.size()) {
            std::size_t slash = path.find('/', i);
            std::size_t end = (slash == std::string::npos) ? path.size() : slash;
            out.push_back(path.substr(i, end - i));
            if (slash == std::string::npos) break;
            i = slash + 1;
        }
        return out;
    }

    // Register a handler, routing it to the parameterized table when the path
    // contains a "{name}" segment and to the exact-match map otherwise. When
    // apply_prefix is false the path is registered verbatim, exempt from the
    // uri_prefix (e.g. the health probe, which must stay at its bare path).
    void add_route(const std::string& method, const std::string& raw_path, HttpHandler h,
                   bool apply_prefix = true) {
        const std::string path = apply_prefix ? with_prefix(raw_path) : raw_path;
        if (path.find('{') == std::string::npos) {
            http_routes[route_key(method, path)] = std::move(h);
            return;
        }
        ParamRoute pr;
        pr.method = method;
        std::vector<std::string> segs = split_segments(path);
        for (std::size_t i = 0; i < segs.size(); ++i) {
            const std::string& s = segs[i];
            const bool param = s.size() >= 2 && s.front() == '{' && s.back() == '}';
            pr.is_param.push_back(param);
            pr.segments.push_back(param ? s.substr(1, s.size() - 2) : s);
        }
        pr.handler = std::move(h);
        param_routes.push_back(std::move(pr));
    }

    // Match a parameterized route, capturing segments into `out`. Returns the
    // handler or nullptr; `out` is only written on a successful match.
    HttpHandler* match_param(const std::string& method, const std::string& path,
                             std::unordered_map<std::string, std::string>& out) {
        std::vector<std::string> parts = split_segments(path);
        for (std::size_t r = 0; r < param_routes.size(); ++r) {
            ParamRoute& pr = param_routes[r];
            if (pr.method != method || pr.segments.size() != parts.size()) continue;

            std::unordered_map<std::string, std::string> caps;
            bool ok = true;
            for (std::size_t i = 0; i < parts.size(); ++i) {
                if (pr.is_param[i]) caps[pr.segments[i]] = parts[i];
                else if (pr.segments[i] != parts[i]) { ok = false; break; }
            }
            if (ok) { out = std::move(caps); return &pr.handler; }
        }
        return nullptr;
    }

    // Resolve a request to a handler: exact match first, then parameterized
    // routes (capturing into `params`). The common case touches only the hash
    // map; `params` is left untouched unless a parameterized route matches.
    HttpHandler* match_http(const std::string& method, const std::string& path,
                            std::unordered_map<std::string, std::string>& params) {
        auto it = http_routes.find(route_key(method, path));
        if (it != http_routes.end()) return &it->second;
        return match_param(method, path, params);
    }

    HttpHandler* find_http(const std::string& method, const std::string& path) {
        auto it = http_routes.find(route_key(method, path));
        return it == http_routes.end() ? nullptr : &it->second;
    }

    // True if `path` is registered under any HTTP method (exact or
    // parameterized). Used to decide whether an OPTIONS request maps to a real
    // route.
    bool has_http_path(const std::string& path) {
        static const char* kVerbs[] = {"GET", "POST", "PUT", "DELETE"};
        for (const char* v : kVerbs) {
            if (http_routes.count(route_key(v, path))) return true;
        }
        std::unordered_map<std::string, std::string> ignore;
        std::vector<std::string> parts = split_segments(path);
        for (std::size_t r = 0; r < param_routes.size(); ++r) {
            if (param_routes[r].segments.size() != parts.size()) continue;
            if (match_param(param_routes[r].method, path, ignore)) return true;
        }
        return false;
    }
};

//------------------------------------------------------------------------------
// Request parsing + header helpers
//------------------------------------------------------------------------------

namespace {

Router::Impl* impl_of(lws* wsi) {
    auto* protocol = lws_get_protocol(wsi);
    if (!protocol) return nullptr;
    return static_cast<Router::Impl*>(protocol->user);
}

// Copy a request header into a std::string, sized off its actual length so
// long values (e.g. bearer JWTs) aren't truncated. Empty when the header is
// absent.
std::string copy_lws_header(lws* wsi, enum lws_token_indexes token) {
    int total = lws_hdr_total_length(wsi, token);
    if (total <= 0) return {};
    std::string out(static_cast<size_t>(total) + 1, '\0');
    int n = lws_hdr_copy(wsi, &out[0], total + 1, token);
    if (n <= 0) return {};
    out.resize(static_cast<size_t>(n));
    return out;
}

// Copy a non-standard (custom) request header that lws doesn't tokenize. `name`
// must include the trailing colon, e.g. "x-timezone:". Empty when absent.
std::string copy_lws_custom_header(lws* wsi, const char* name) {
    int nlen = static_cast<int>(std::strlen(name));
    int total = lws_hdr_custom_length(wsi, name, nlen);
    if (total <= 0) return {};
    std::string out(static_cast<size_t>(total) + 1, '\0');
    int n = lws_hdr_custom_copy(wsi, &out[0], total + 1, name, nlen);
    if (n <= 0) return {};
    out.resize(static_cast<size_t>(n));
    return out;
}

std::string trim(std::string s) {
    auto is_ws = [](char c) { return c == ' ' || c == '\t'; };
    size_t b = 0;
    while (b < s.size() && is_ws(s[b])) ++b;
    size_t e = s.size();
    while (e > b && is_ws(s[e - 1])) --e;
    return s.substr(b, e - b);
}

// First (highest-priority) language tag of an Accept-Language header, with any
// ";q=" weight stripped: "en-US,en;q=0.9,fr" -> "en-US". Empty input -> empty.
std::string first_language(std::string header) {
    size_t comma = header.find(',');
    std::string first = (comma == std::string::npos) ? header : header.substr(0, comma);
    size_t semi = first.find(';');
    if (semi != std::string::npos) first.resize(semi);
    return trim(std::move(first));
}

// Parse a dotted-quad IPv4 literal into four octets, rejecting anything that
// isn't exactly four 0-255 decimal groups. Returns false (and leaves `out`
// unspecified) on a malformed value.
bool parse_ipv4(const std::string& s, unsigned out[4]) {
    unsigned v = 0;
    int digits = 0, oct = 0;
    for (size_t i = 0; i <= s.size(); ++i) {
        if (i < s.size() && s[i] >= '0' && s[i] <= '9') {
            v = v * 10 + static_cast<unsigned>(s[i] - '0');
            if (++digits > 3 || v > 255) return false;
        } else if (i == s.size() || s[i] == '.') {
            if (digits == 0 || oct > 3) return false;
            out[oct++] = v;
            v = 0; digits = 0;
            if (i == s.size()) break;
        } else {
            return false;
        }
    }
    return oct == 4;
}

// Whether `ip` is a publicly routable address: not loopback, private, link-
// local, carrier-grade NAT, multicast, or otherwise reserved. Used to pick the
// real client out of an X-Forwarded-For chain, skipping internal hops. An
// unparseable value is treated as non-public.
bool is_public_ip(const std::string& ip) {
    if (ip.empty()) return false;

    if (ip.find(':') != std::string::npos) {
        // IPv6 (or an IPv4-mapped form). Reject loopback and the unspecified
        // address outright; judge an ::ffff: mapped address by its IPv4 tail.
        if (ip == "::1" || ip == "::") return false;
        const char* mapped = "::ffff:";
        size_t mlen = std::strlen(mapped);
        if (ip.size() > mlen && ip.compare(0, mlen, mapped) == 0 &&
            ip.find('.') != std::string::npos) {
            return is_public_ip(ip.substr(mlen));
        }
        size_t colon = ip.find(':');
        if (colon == 0) return true;   // "::xxxx" global form, not ULA/link-local
        unsigned long first = std::strtoul(ip.substr(0, colon).c_str(), nullptr, 16);
        if ((first & 0xfe00) == 0xfc00) return false;   // fc00::/7 unique-local
        if ((first & 0xffc0) == 0xfe80) return false;   // fe80::/10 link-local
        return true;
    }

    unsigned o[4];
    if (!parse_ipv4(ip, o)) return false;
    if (o[0] == 0)                          return false;   // 0.0.0.0/8
    if (o[0] == 10)                         return false;   // 10.0.0.0/8
    if (o[0] == 127)                        return false;   // loopback
    if (o[0] == 169 && o[1] == 254)         return false;   // link-local
    if (o[0] == 172 && o[1] >= 16 && o[1] <= 31) return false; // 172.16.0.0/12
    if (o[0] == 192 && o[1] == 168)         return false;   // 192.168.0.0/16
    if (o[0] == 100 && o[1] >= 64 && o[1] <= 127) return false; // 100.64.0.0/10 CGNAT
    if (o[0] >= 224)                        return false;   // multicast / reserved
    return true;
}

// Resolve the client IP, preferring proxy-forwarded addresses over the direct
// peer. X-Forwarded-For holds "client, proxy1, proxy2"; the leftmost entries
// can be private internal hops or client-spoofed, so return the first publicly
// routable one, falling back to the leftmost entry if none are public. X-Real-IP
// is a single address. Both are set by an upstream proxy and are spoofable by a
// client connecting directly, so only the peer fallback is trustworthy without a
// trusted proxy in front.
std::string client_ip(lws* wsi) {
    std::string xff = copy_lws_header(wsi, WSI_TOKEN_X_FORWARDED_FOR);
    if (!xff.empty()) {
        std::string leftmost;
        size_t i = 0;
        while (i < xff.size()) {
            size_t comma = xff.find(',', i);
            size_t end = (comma == std::string::npos) ? xff.size() : comma;
            std::string entry = trim(xff.substr(i, end - i));
            if (!entry.empty()) {
                if (is_public_ip(entry)) return entry;
                if (leftmost.empty()) leftmost = entry;
            }
            if (comma == std::string::npos) break;
            i = comma + 1;
        }
        if (!leftmost.empty()) return leftmost;
    }

    std::string xri = trim(copy_lws_header(wsi, WSI_TOKEN_HTTP_X_REAL_IP));
    if (!xri.empty()) return xri;

    char ip[64] = {0};
    if (lws_get_peer_simple(wsi, ip, sizeof(ip))) return ip;
    return {};
}

// Whether the request carries a body, so the router knows to wait for
// HTTP_BODY_COMPLETION rather than dispatching at headers time. True when
// Transfer-Encoding is present (chunked) or Content-Length > 0. A body-less
// POST/PUT (e.g. `POST /logout`) returns false and is dispatched immediately,
// just like GET/DELETE.
bool request_has_body(lws* wsi) {
    if (lws_hdr_total_length(wsi, WSI_TOKEN_HTTP_TRANSFER_ENCODING) > 0) return true;
    char cl[32] = {0};
    int n = lws_hdr_copy(wsi, cl, sizeof(cl), WSI_TOKEN_HTTP_CONTENT_LENGTH);
    if (n > 0) return std::strtol(cl, nullptr, 10) > 0;
    return false;
}

// Extract just the request method and path (query split off) from the lws
// header table -- the cheap subset of collect_request, with none of the extra
// header copies/allocations. Enough on its own for the health probe fast path,
// which only compares method and path.
void collect_method_path(lws* wsi, Request& req) {
    char buf[1024];

    int len = lws_hdr_copy(wsi, buf, sizeof(buf), WSI_TOKEN_GET_URI);
    if (len > 0) {
        req.method = "GET"; req.path.assign(buf, len);
    }
    else if ((len = lws_hdr_copy(wsi, buf, sizeof(buf), WSI_TOKEN_POST_URI)) > 0) {
        req.method = "POST"; req.path.assign(buf, len);
    }
    else if ((len = lws_hdr_copy(wsi, buf, sizeof(buf), WSI_TOKEN_PUT_URI)) > 0) {
        req.method = "PUT"; req.path.assign(buf, len);
    }
    else if ((len = lws_hdr_copy(wsi, buf, sizeof(buf), WSI_TOKEN_DELETE_URI)) > 0) {
        req.method = "DELETE"; req.path.assign(buf, len);
    }
    else if ((len = lws_hdr_copy(wsi, buf, sizeof(buf), WSI_TOKEN_OPTIONS_URI)) > 0) {
        req.method = "OPTIONS"; req.path.assign(buf, len);
    }

    // Some transports leave the query inline on the URI; split it off if so.
    auto p = req.path.find('?');
    if (p != std::string::npos) {
        req.query = req.path.substr(p + 1);
        req.path.resize(p);
    }

    // lws, however, parses the query OUT of the method-URI token (above is
    // path-only) and stores it in WSI_TOKEN_HTTP_URI_ARGS, tokenized by '&' into
    // one fragment per arg. Reconstruct the raw "k=v&k=v" string the rest of the
    // router reads via req.query_str() -- without this, req.query is always empty
    // and e.g. presigned "?expires=&sig=" URLs all fail verification as unsigned.
    if (req.query.empty()) {
        std::string q;
        for (int frag = 0; ; ++frag) {
            const int fl = lws_hdr_copy_fragment(wsi, buf, sizeof(buf),
                                                 WSI_TOKEN_HTTP_URI_ARGS, frag);
            if (fl <= 0) break;
            if (!q.empty()) q.push_back('&');
            q.append(buf, static_cast<std::size_t>(fl));
        }
        req.query = std::move(q);
    }
}

// Populate the rest of the Request: the headers and derived client metadata that
// handlers/middleware need. Assumes collect_method_path already ran (the query
// is used for the query-param fallbacks below).
void collect_headers(lws* wsi, Request& req) {
    // Surface the headers handlers/middleware need: Authorization for auth,
    // User-Agent and Accept-Language for client metadata, Host so handlers can
    // build absolute URLs back to themselves (e.g. the personal MCP URL).
    req.authorization   = copy_lws_header(wsi, WSI_TOKEN_HTTP_AUTHORIZATION);
    req.user_agent      = copy_lws_header(wsi, WSI_TOKEN_HTTP_USER_AGENT);
    req.host            = copy_lws_header(wsi, WSI_TOKEN_HOST);
    req.content_type    = copy_lws_header(wsi, WSI_TOKEN_HTTP_CONTENT_TYPE);
    req.accept_encoding = copy_lws_header(wsi, WSI_TOKEN_HTTP_ACCEPT_ENCODING);

    req.accept_language = first_language(copy_lws_header(wsi, WSI_TOKEN_HTTP_ACCEPT_LANGUAGE));

    // Non-standard, set by the frontend (e.g. from Intl.DateTimeFormat()).
    req.timezone = copy_lws_custom_header(wsi, "x-timezone:");

    // Names the last SSE event the client saw, so a stream can resume where it
    // left off; the matching resumable session is named by session_id. Both may
    // arrive in a custom header or the query string (the latter for clients that
    // can't set headers); the header wins when both are present.
    req.last_event_id = copy_lws_custom_header(wsi, "x-last-event-id:");
    // URL-safe values (numeric event id, hex session id), so the verbatim
    // (undecoded) query_str is what we want.
    if (req.last_event_id.empty()) req.last_event_id = req.query_str("last_event_id", "");

    req.session_id = copy_lws_custom_header(wsi, "x-session-id:");
    if (req.session_id.empty()) req.session_id = req.query_str("session_id", "");

    // Client IP, preferring proxy-forwarded headers over the direct peer.
    req.ip = client_ip(wsi);
}

// Full request parse: method/path plus all the headers and derived metadata.
void collect_request(lws* wsi, Request& req) {
    collect_method_path(wsi, req);
    collect_headers(wsi, req);
}

//------------------------------------------------------------------------------
// Static file serving
//------------------------------------------------------------------------------

// Percent-decode a URL *path*: "%20" -> ' ', "%2F" -> '/', leaving any malformed
// escape untouched. '+' stays literal (it means space only in a query string),
// hence percent_decode's plus_as_space=false.
std::string url_decode(const std::string& s) {
    return Request::percent_decode(s, /*plus_as_space=*/false);
}

bool is_regular_file(const std::string& path) {
    struct stat st;
    if (stat(path.c_str(), &st) != 0) return false;
    return (st.st_mode & S_IFREG) != 0;
}

// Map a URL path under `root` to a safe filesystem path, or empty on rejection.
// A trailing "/" (or the bare "/") resolves to index.html. Any segment that is
// empty, ".", "..", or contains a backslash or NUL is rejected, which blocks
// directory traversal out of `root` and Windows separator tricks. When
// `out_rel` is non-null it receives the canonical, decoded relative path that
// `full` is `root` + "/" + it -- the object key the storage mount signs over.
std::string resolve_static_path(const std::string& root, const std::string& url_path,
                                std::string* out_rel = nullptr) {
    std::string decoded = url_decode(url_path);
    if (decoded.empty() || decoded[0] != '/') return {};
    if (decoded.find('\0') != std::string::npos) return {};

    std::string rel;
    size_t i = 1;  // skip leading '/'
    while (i <= decoded.size()) {
        size_t slash = decoded.find('/', i);
        std::string seg = decoded.substr(i, slash == std::string::npos ? std::string::npos : slash - i);
        if (seg == "." || seg == "..") return {};
        if (seg.find('\\') != std::string::npos) return {};
        if (!seg.empty()) {
            if (!rel.empty()) rel.push_back('/');
            rel += seg;
        }
        if (slash == std::string::npos) break;
        i = slash + 1;
    }

    // A directory request ("/", "/foo/") serves its index.html.
    if (rel.empty() || decoded.back() == '/') {
        if (!rel.empty()) rel.push_back('/');
        rel += "index.html";
    }

    if (out_rel) *out_rel = rel;

    std::string full = root;
    if (!full.empty() && full.back() != '/' && full.back() != '\\') full.push_back('/');
    full += rel;
    return full;
}

// Try to serve `url_path` as a static file from one of impl->http_roots.
// Returns true when the request was handled (and writes the callback's return
// code to *rc); false when no matching file exists in any mount, so the caller
// falls through to its 404.
bool serve_static_file(lws* wsi, Router::Impl* impl, const std::string& url_path, int* rc) {
    if (impl->http_roots.empty()) return false;

    // The whole app is served under uri_prefix, but files on disk live at the
    // root of each mount, so drop the prefix before resolving. A path not under
    // the prefix yields "" -> serve nothing (404).
    const std::string stripped = impl->strip_prefix(url_path);
    if (stripped.empty()) return false;

    // Mounts are ordered most-specific-prefix-first, so the first that owns the
    // path and holds the file wins; a deeper mount that lacks the file falls
    // through to the "/" catch-all.
    std::string path;
    for (const auto& mount : impl->http_roots) {
        const std::string& mp = mount.first;
        std::string rel;
        if (mp == "/") {
            rel = stripped;                       // root mount owns everything
        } else if (stripped == mp) {
            rel = "/";                            // bare mount root -> index.html
        } else if (stripped.size() > mp.size() &&
                   stripped.compare(0, mp.size(), mp) == 0 &&
                   stripped[mp.size()] == '/') {
            rel = stripped.substr(mp.size());     // "/dashboard/app.js" -> "/app.js"
        } else {
            continue;                             // not under this mount
        }
        std::string candidate = resolve_static_path(mount.second, rel);
        if (!candidate.empty() && is_regular_file(candidate)) { path = candidate; break; }
    }
    if (path.empty()) return false;

    const char* mime = lws_get_mimetype(path.c_str(), nullptr);
    // The web build emits unhashed asset names (assets/index.js, ...), so tell
    // the browser to revalidate rather than serve a stale cached copy after a
    // rebuild. lws still sends Last-Modified and answers If-Modified-Since with
    // a 304, so an unchanged file is a cheap revalidation, not a full refetch.
    static const char kCacheHeader[] = "Cache-Control: no-cache\x0d\x0a";
    int r = lws_serve_http_file(wsi, path.c_str(),
                                mime ? mime : "application/octet-stream",
                                kCacheHeader, sizeof(kCacheHeader) - 1);
    // >0: file fully sent and transaction completed -> close. ==0: transfer
    // started, lws drives the rest via FILE_COMPLETION -> leave the wsi alone.
    // <0: error -> close.
    *rc = (r < 0) ? -1 : (r > 0 ? -1 : 0);
    return true;
}

// Constant-time string equality, so comparing a request's signature against the
// expected one can't leak how many leading bytes matched via early-exit timing.
// Both operands are fixed-length lowercase hex here, so the length check up
// front reveals nothing secret.
bool ct_equals(const std::string& a, const std::string& b) {
    if (a.size() != b.size()) return false;
    unsigned char diff = 0;
    for (std::size_t i = 0; i < a.size(); ++i) {
        diff |= static_cast<unsigned char>(a[i] ^ b[i]);
    }
    return diff == 0;
}

// Try to serve `req` from the decrypting file mount (FILE_ENCRYPTION_KEY).
// Returns false when the mount is disabled or the path is not under it, so the
// caller continues to the disk mount / static fallback. Otherwise the request
// is ours: on success it fills `session->response` with the decrypted bytes and
// arms the writeable callback (the same path an ordinary handler takes); a
// bad/expired/forged signature is a 403 and a missing object a 404, written
// directly. See Router::set_file_mount.
bool serve_decrypted_file(lws* wsi, Router::Impl* impl, PerHttpSession* session,
                          const Request& req, int* rc) {
    if (impl->file_mount.empty() || impl->file_storage == nullptr) return false;

    const std::string url_path = impl->strip_prefix(req.path);
    const std::string& mp = impl->file_mount;
    if (!(url_path.size() > mp.size() &&
          url_path.compare(0, mp.size(), mp) == 0 &&
          url_path[mp.size()] == '/')) {
        return false;
    }
    // The object key is the rest of the path, percent-decoded (signed_read_url
    // uri_encode()s it). It is a FULL key, configured storage prefix included.
    const std::string key = url_decode(url_path.substr(mp.size() + 1));

    auto deny = [&](int status, const char* msg) -> bool {
        lws_return_http_status(wsi, status, msg);
        *rc = -1;
        return true;
    };

    // The signature is the capability: a valid, unexpired HMAC over the exact
    // key grants the read. Storage::signed_read_url mints it; the key's hashed
    // user segment already scopes it to the one user it was minted for.
    const std::string expires = req.query_str("expires", "");
    const std::string sig     = req.query_str("sig", "");
    if (expires.empty() || sig.empty()) return deny(HTTP_STATUS_FORBIDDEN, "unsigned");
    const long long exp = std::strtoll(expires.c_str(), nullptr, 10);
    if (exp <= 0 || static_cast<std::time_t>(exp) <= std::time(nullptr)) {
        return deny(HTTP_STATUS_FORBIDDEN, "expired");
    }
    const std::string to_sign = key + "\n" + expires;
    const std::array<unsigned char, 32> mac = storage::hmac_sha256(impl->file_secret, to_sign);
    const std::string want = storage::hex_encode(mac.data(), mac.size());
    if (!ct_equals(want, sig)) return deny(HTTP_STATUS_FORBIDDEN, "bad signature");

    std::string body;
    try {
        body = impl->file_storage->get_object_decrypted(key);
    } catch (const storage::StorageError&) {
        return deny(HTTP_STATUS_NOT_FOUND, "not found");
    }

    // A .trans sidecar is extracted text; every other upload key carries a
    // suffix derived from its content-type at upload, so the extension resolves
    // back to the right type.
    const std::string ctype = storage::is_trans_key(key)
        ? std::string("text/plain; charset=utf-8")
        : storage::resolve_content_type(key, "");

    session->response.status(200);
    session->response.content_type(ctype);
    session->response.body(std::move(body));
    session->response_ready = true;
    lws_callback_on_writable(wsi);
    *rc = 0;
    return true;
}

// Try to serve `req` from the LocalStorage HTTP mount. Returns false when the
// mount is disabled or the path is not under it, so the caller continues to the
// static fallback / 404. Otherwise the request belongs to the mount and is
// handled here in full (writing *rc): the file with a 200, 403 on a missing /
// expired / forged signature, or 404 when no such object exists. The object
// directory need not live under http_root.
bool serve_storage_file(lws* wsi, Router::Impl* impl, const Request& req, int* rc) {
    if (impl->storage_mount.empty() || impl->storage_dir.empty()) return false;

    // Resolve against the path with any HTTP_URI_PREFIX removed, matching how
    // static files resolve. (No prefix configured => unchanged.)
    const std::string url_path = impl->strip_prefix(req.path);

    // Only paths strictly under the mount ("<mount>/...") belong to it; the bare
    // mount path itself carries no object key and is left to fall through.
    const std::string& mp = impl->storage_mount;
    if (!(url_path.size() > mp.size() &&
          url_path.compare(0, mp.size(), mp) == 0 &&
          url_path[mp.size()] == '/')) {
        return false;
    }
    // url_prefix is a URL path, not an on-disk prefix: strip it to recover the
    // object key, which maps directly under storage_dir.
    const std::string rel = url_path.substr(mp.size());   // e.g. "/x.png"

    // `key` is the canonical decoded object key (e.g. "x.png") -- exactly what
    // LocalStorage::presigned_url signed and what `path` resolves to, so the
    // bytes we verify and the bytes we serve can never diverge.
    std::string key;
    const std::string path = resolve_static_path(impl->storage_dir, rel, &key);

    // From here the request is ours: every failure is a storage 403/404, never a
    // fall-through that could leak the lookup into the static root.
    auto deny = [&](int status, const char* msg) -> bool {
        lws_return_http_status(wsi, status, msg);
        *rc = -1;
        return true;
    };

    // Verify the signature before touching the filesystem, so an unsigned probe
    // gets a uniform 403 whether or not the object exists.
    if (!impl->storage_secret.empty()) {
        const std::string expires = req.query_str("expires", "");
        const std::string sig     = req.query_str("sig", "");
        if (expires.empty() || sig.empty()) {
            platform::log_debug("storage[verify]: unsigned key='%s' (expires/sig missing)", key.c_str());
            return deny(HTTP_STATUS_FORBIDDEN, "unsigned");
        }

        // expires is an absolute unix time; reject once it has passed.
        const long long exp = std::strtoll(expires.c_str(), nullptr, 10);
        const long long now = static_cast<long long>(std::time(nullptr));
        if (exp <= 0 || static_cast<std::time_t>(exp) <= std::time(nullptr)) {
            platform::log_debug("storage[verify]: expired key='%s' exp=%lld now=%lld (%lld s ago)",
                                key.c_str(), exp, now, now - exp);
            return deny(HTTP_STATUS_FORBIDDEN, "expired");
        }

        // Recompute the same HMAC presigned_url minted over "<key>\n<expires>".
        const std::string to_sign = key + "\n" + expires;
        const std::array<unsigned char, 32> mac = storage::hmac_sha256(impl->storage_secret, to_sign);
        const std::string want = storage::hex_encode(mac.data(), mac.size());
        if (!ct_equals(want, sig)) {
            platform::log_debug("storage[verify]: bad signature key='%s' expires=%s want=%s got=%s",
                                key.c_str(), expires.c_str(), want.c_str(), sig.c_str());
            return deny(HTTP_STATUS_FORBIDDEN, "bad signature");
        }
    }

    if (path.empty() || !is_regular_file(path)) return deny(HTTP_STATUS_NOT_FOUND, "not found");

    const char* mime = lws_get_mimetype(path.c_str(), nullptr);
    int r = lws_serve_http_file(wsi, path.c_str(),
                                mime ? mime : "application/octet-stream", nullptr, 0);
    *rc = (r < 0) ? -1 : (r > 0 ? -1 : 0);
    return true;
}

//------------------------------------------------------------------------------
// Response writing
//------------------------------------------------------------------------------

// Body below this size is sent uncompressed: gzip's framing overhead and the
// CPU to produce it don't pay off, and tiny payloads can even grow.
constexpr size_t GZIP_MIN_BODY = 10 * 1024;

// Whether the client's Accept-Encoding advertises gzip as acceptable. A plain
// substring match, except we reject an explicit "gzip;q=0" ("not acceptable");
// finer q-value ranking isn't worth it for a single supported encoding.
bool client_accepts_gzip(const std::string& accept_encoding) {
    const std::size_t g = accept_encoding.find("gzip");
    if (g == std::string::npos) return false;
    // Look for a ";q=0" qualifier attached to this token (up to the next comma),
    // treating "q=0", "q=0.0" as a refusal but "q=0.5" as acceptance.
    const std::size_t comma = accept_encoding.find(',', g);
    const std::string tok = accept_encoding.substr(g, comma == std::string::npos ? std::string::npos : comma - g);
    const std::size_t q = tok.find("q=");
    if (q != std::string::npos) {
        const std::string qv = tok.substr(q + 2);
        if (qv.compare(0, 1, "0") == 0 && qv.find_first_of("123456789") == std::string::npos) return false;
    }
    return true;
}

// Whether a response of this content-type compresses well enough to bother:
// text-like payloads (HTML/CSS/JS/JSON/XML/SVG). Binary and already-compressed
// types (images, archives, fonts) are skipped -- gzip just burns CPU on them.
bool gzip_worthwhile_type(const std::string& content_type) {
    std::string ct = content_type;
    for (std::size_t i = 0; i < ct.size(); ++i) {
        char& c = ct[i];
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    }
    return ct.compare(0, 5, "text/") == 0 ||
           ct.find("json") != std::string::npos ||
           ct.find("javascript") != std::string::npos ||
           ct.find("xml") != std::string::npos ||
           ct.find("svg") != std::string::npos;
}

// gzip-compress `in` into a self-contained gzip stream (RFC 1952) for a
// Content-Encoding: gzip response. Returns true and fills `out` on success;
// false on any zlib error, so the caller can fall back to the raw body.
bool gzip_compress(const std::string& in, std::string& out) {
    z_stream zs;
    std::memset(&zs, 0, sizeof(zs));
    // windowBits 15|16: 15 = the maximum (32 KiB) window, +16 selects the gzip
    // wrapper (as opposed to the zlib wrapper used by the lexicon artifact).
    if (deflateInit2(&zs, Z_DEFAULT_COMPRESSION, Z_DEFLATED, 15 | 16, 8,
                     Z_DEFAULT_STRATEGY) != Z_OK) {
        return false;
    }
    zs.next_in  = reinterpret_cast<Bytef*>(const_cast<char*>(in.data()));
    zs.avail_in = static_cast<uInt>(in.size());
    out.resize(static_cast<std::size_t>(deflateBound(&zs, static_cast<uLong>(in.size()))));
    zs.next_out  = reinterpret_cast<Bytef*>(&out[0]);
    zs.avail_out = static_cast<uInt>(out.size());
    const int rc = deflate(&zs, Z_FINISH);
    const uLong produced = zs.total_out;
    deflateEnd(&zs);
    if (rc != Z_STREAM_END) return false;   // didn't fit in one pass -- give up
    out.resize(static_cast<std::size_t>(produced));
    return true;
}

// Append the response's per-request headers and the router's configured default
// headers (the CORS Access-Control-* set) to an in-progress lws header block.
// Returns non-zero on buffer overflow, so callers can propagate the lws failure.
int add_extra_headers(lws* wsi, const Response& res, Router::Impl* impl, uint8_t** p, uint8_t* end) {
    for (auto _hit = res.headers().begin(); _hit != res.headers().end(); ++_hit) {
        const std::string field = _hit->first + ":";
        if (lws_add_http_header_by_name(wsi,
                reinterpret_cast<const unsigned char*>(field.c_str()),
                reinterpret_cast<const unsigned char*>(_hit->second.c_str()),
                static_cast<int>(_hit->second.size()), p, end)) return -1;
    }
    if (impl) {
        for (auto _dit = impl->default_headers.begin(); _dit != impl->default_headers.end(); ++_dit) {
            const std::string field = _dit->first + ":";
            if (lws_add_http_header_by_name(wsi,
                    reinterpret_cast<const unsigned char*>(field.c_str()),
                    reinterpret_cast<const unsigned char*>(_dit->second.c_str()),
                    static_cast<int>(_dit->second.size()), p, end)) return -1;
        }
    }
    return 0;
}

// Drive a chunked (streaming) HTTP response from the writeable callback. The
// first call writes the status line and headers with an unknown content length,
// so lws frames the body with Transfer-Encoding: chunked. Each later call writes
// as much of the current chunk as lws accepts and, when the chunk is drained,
// pulls the next one from the producer; when the producer signals end-of-stream
// a zero-length LWS_WRITE_HTTP_FINAL emits the terminating chunk. The callback
// re-arms itself after every partial write, so a chunk larger than the transmit
// buffer is sent across several callbacks.
int write_streaming(lws* wsi, PerHttpSession* session, Router::Impl* impl) {
    Response& res = session->response;

    if (!session->headers_sent) {
        uint8_t headers[LWS_TX_BUFFER];
        uint8_t* p = headers + LWS_PRE;
        uint8_t* end = headers + sizeof(headers);

        if (lws_add_http_common_headers(wsi,
                static_cast<unsigned>(res.status()),
                res.content_type().c_str(),
                LWS_ILLEGAL_HTTP_CONTENT_LEN, &p, end)) return -1;
        if (add_extra_headers(wsi, res, impl, &p, end)) return -1;
        if (lws_finalize_write_http_header(wsi, headers + LWS_PRE, &p, end)) return -1;

        session->headers_sent = true;
        lws_callback_on_writable(wsi);
        return 0;
    }

    // Refill: once the current chunk is fully sent, pull the next non-empty
    // chunk, skipping any empty true-returns, until the producer is exhausted.
    if (session->stream_off >= session->stream_chunk.size()) {
        session->stream_chunk.clear();
        session->stream_off = 0;
        while (!session->producer_done && session->stream_chunk.empty()) {
            std::string out;
            if (res.producer()(out)) session->stream_chunk = std::move(out);
            else                     session->producer_done = true;
        }
    }

    // Exhausted: close the chunked body with a zero-length final chunk.
    if (session->producer_done && session->stream_chunk.empty()) {
        uint8_t fin[LWS_PRE + 1];
        if (lws_write(wsi, fin + LWS_PRE, 0, LWS_WRITE_HTTP_FINAL) < 0) return -1;
        return lws_http_transaction_completed(wsi) ? -1 : 0;
    }

    size_t remaining = session->stream_chunk.size() - session->stream_off;
    size_t n = std::min<size_t>(remaining, LWS_TX_BUFFER - LWS_PRE);
    std::vector<uint8_t> buf(LWS_PRE + n);
    std::memcpy(buf.data() + LWS_PRE, session->stream_chunk.data() + session->stream_off, n);

    int written = lws_write(wsi, buf.data() + LWS_PRE, n, LWS_WRITE_HTTP);
    if (written < 0) return -1;
    session->stream_off += static_cast<size_t>(written);

    lws_callback_on_writable(wsi);
    return 0;
}

// Drive an asynchronous (StreamWriter-backed) chunked response. The producer
// runs on a worker thread and wakes this callback through lws_cancel_service ->
// EVENT_WAIT_CANCELLED, so each call sends whatever is buffered and then, if the
// producer isn't finished, returns without re-arming -- it waits for the next
// wakeup rather than spinning. Headers are emitted (with the producer's status
// and Content-Type) only once the producer has called begin().
int write_async_stream(lws* wsi, PerHttpSession* session, Router::Impl* impl) {
    const std::shared_ptr<StreamWriter>& sw = session->response.stream_writer();
    if (!sw) return -1;

    if (!session->headers_sent) {
        if (!sw->headers_ready()) return 0;     // producer hasn't begun(); wait for wakeup

        uint8_t headers[LWS_TX_BUFFER];
        uint8_t* p = headers + LWS_PRE;
        uint8_t* end = headers + sizeof(headers);

        if (lws_add_http_common_headers(wsi,
                static_cast<unsigned>(sw->status()),
                sw->content_type().c_str(),
                LWS_ILLEGAL_HTTP_CONTENT_LEN, &p, end)) return -1;
        if (add_extra_headers(wsi, session->response, impl, &p, end)) return -1;
        if (lws_finalize_write_http_header(wsi, headers + LWS_PRE, &p, end)) return -1;

        session->headers_sent = true;
        lws_callback_on_writable(wsi);
        return 0;
    }

    // Refill from the writer once the current slice is drained. take() reports
    // end-of-stream atomically with the buffer read.
    bool done = false;
    if (session->stream_off >= session->stream_chunk.size()) {
        session->stream_chunk.clear();
        session->stream_off = 0;
        done = sw->take(session->stream_chunk);
    }

    if (session->stream_chunk.empty()) {
        if (done) {
            impl->active_streams.erase(wsi);
            // fail(): the producer ended the stream broken (e.g. the upstream
            // socket dropped mid-response). Close the connection WITHOUT the
            // terminating chunk so the client sees an incomplete chunked body and
            // surfaces an error, rather than mistaking truncation for success.
            if (sw->failed()) return -1;

            // We arrived here with no data left to carry the FINAL flag -- the
            // last bytes were already written (as LWS_WRITE_HTTP) on an earlier
            // callback, because the producer signalled end-of-stream separately
            // from its final write() (write()+finish() instead of the atomic
            // finish(final_bytes)). A zero-length LWS_WRITE_HTTP_FINAL does NOT
            // reliably emit the terminating chunk in lws, so fall back to
            // transaction-completion; producers that send their tail via
            // finish(final_bytes) take the FINAL-on-data path below and never get
            // here.
            return lws_http_transaction_completed(wsi) ? -1 : 0;
        }
        // Nothing buffered yet; the next cancel wakeup re-arms us. The gap can be
        // long and silent -- a thinking model (e.g. Gemini) deliberates for
        // seconds before its first token, producing no body bytes meanwhile. lws
        // arms a pending timeout when it sends the response headers and reaps the
        // connection if the body stalls; here the stall is intentional, so cancel
        // that timeout. Left armed, it closes the socket without the terminating
        // chunk and the client sees ERR_INCOMPLETE_CHUNKED_ENCODING. A real client
        // disconnect still arrives via HTTP_DROP_PROTOCOL, and the producer's own
        // request timeout bounds the wait, so this can't leak a stuck stream.
        lws_set_timeout(wsi, NO_PENDING_TIMEOUT, 0);
        return 0;
    }

    size_t remaining = session->stream_chunk.size() - session->stream_off;
    size_t n = std::min<size_t>(remaining, LWS_TX_BUFFER - LWS_PRE);
    std::vector<uint8_t> buf(LWS_PRE + n);
    std::memcpy(buf.data() + LWS_PRE, session->stream_chunk.data() + session->stream_off, n);

    // When the producer is finished and this write drains the last buffered
    // bytes, mark it LWS_WRITE_HTTP_FINAL so lws appends the chunked terminator
    // as part of this very write -- the reliable way to close a chunked body.
    const bool last = done && (n == remaining);
    int written = lws_write(wsi, buf.data() + LWS_PRE, n,
                            last ? LWS_WRITE_HTTP_FINAL : LWS_WRITE_HTTP);
    if (written < 0) return -1;
    session->stream_off += static_cast<size_t>(written);

    // A full FINAL write completes the transaction; complete it and stop. (A
    // short write can't happen here: n is capped to one tx buffer, which lws
    // accepts whole.)
    if (last && static_cast<size_t>(written) == n) {
        impl->active_streams.erase(wsi);
        return lws_http_transaction_completed(wsi) ? -1 : 0;
    }

    // Re-arm to drain the rest of this chunk and then check for more / finalize.
    lws_callback_on_writable(wsi);
    return 0;
}

//------------------------------------------------------------------------------
// libwebsockets protocol callbacks
//------------------------------------------------------------------------------

int callback_http(lws* wsi, enum lws_callback_reasons reason,
                  void* user, void* in, size_t len) {
    auto* session = static_cast<PerHttpSession*>(user);
    auto* impl = impl_of(wsi);

    switch (reason) {
    case LWS_CALLBACK_HTTP: {
        new (session) PerHttpSession();

        // Only method + path so far -- the cheap parse. The health probe needs
        // nothing more, so parse the rest of the headers lazily after the fast
        // path below bails out.
        Request req;
        collect_method_path(wsi, req);

        // Health probe fast path: answer GET /api/health directly and return,
        // bypassing the header parse, OPTIONS/CORS, body handling, route lookup,
        // and the static fallback. Probes hit this every few seconds, so keep it
        // as cheap as possible. (The endpoint is also registered via
        // get_unprefixed, which remains the canonical route and keeps OPTIONS
        // preflight working; this just short-circuits the common GET.) Returns
        // before the access log, so probe traffic does not flood it either.
        if (req.method == "GET" && req.path == "/api/health") {
            session->response.text("ok");
            session->response_ready = true;
            lws_callback_on_writable(wsi);
            return 0;
        }

        // Not a health probe: finish parsing the request and bind the connection
        // so a handler can start an async stream (begin_stream needs the lws
        // context for cross-thread wakeups). The same response object serves both
        // the immediate and the body-completion dispatch below.
        collect_headers(wsi, req);
        session->response.bind(wsi, lws_get_context(wsi));

        // Per-request access log. Fires once per request at headers time, for
        // every method (including OPTIONS), before routing. Gated on LOG_LEVEL:
        // silent unless the level is debug.
        platform::log_debug("http %s %s%s%s from %s \"%s\"",
            req.method.c_str(), req.path.c_str(),
            req.query.empty() ? "" : "?", req.query.c_str(),
            req.ip.empty() ? "-" : req.ip.c_str(),
            req.user_agent.empty() ? "-" : req.user_agent.c_str());

        // CORS preflight: when default headers are configured (the
        // Access-Control-* set from HTTP_HEADERS), answer any OPTIONS centrally
        // with 204. The headers themselves are injected for every response in
        // the WRITEABLE handler below.
        if (req.method == "OPTIONS" && impl && !impl->default_headers.empty()) {
            // Only answer preflight for a route that actually exists, so OPTIONS
            // to an unknown path 404s instead of returning a misleading 204. A
            // real CORS preflight names the intended verb in the
            // Access-Control-Request-Method header; without it, accept any
            // method registered on the path.
            std::string acrm = trim(copy_lws_custom_header(wsi, "access-control-request-method:"));
            bool exists = acrm.empty() ? impl->has_http_path(req.path)
                                       : impl->match_http(acrm, req.path, req.path_params) != nullptr;
            if (!exists) {
                return lws_http_transaction_completed(wsi) ? -1 :
                    (lws_return_http_status(wsi, HTTP_STATUS_NOT_FOUND, "not found"), -1);
            }
            session->response.status(204);
            session->response_ready = true;
            lws_callback_on_writable(wsi);
            return 0;
        }

        // Requests with a body (POST/PUT with content, chunked uploads, or even
        // DELETE with a payload) are dispatched once the body arrives, in
        // HTTP_BODY_COMPLETION. Everything else — GET, DELETE, and body-less
        // POST/PUT like `POST /logout` — is dispatched here immediately, since
        // lws won't deliver a body-completion callback for them.
        if (request_has_body(wsi)) return 0;

        auto* h = impl ? impl->match_http(req.method, req.path, req.path_params) : nullptr;
        if (!h) {
            // The mount root requested without a trailing slash ("/mirobody")
            // redirects to "/mirobody/". Without the slash the browser treats the
            // last path segment as a file and resolves the SPA's relative asset
            // URLs against the parent ("/assets/index.js" -> 404); the slash makes
            // the document base the mount, so they resolve under it. Mirrors the
            // trailing-slash redirect every static web server does for a dir.
            if (impl && req.method == "GET" && !impl->uri_prefix.empty() &&
                req.path == impl->uri_prefix) {
                std::string loc = impl->uri_prefix + "/";
                if (!req.query.empty()) { loc += "?"; loc += req.query; }
                session->response.status(302);
                session->response.header("Location", loc);
                session->response_ready = true;
                lws_callback_on_writable(wsi);
                return 0;
            }

            // Same trailing-slash redirect for the bare root of a sub-mount
            // ("/dashboard" -> "/dashboard/"), so a frontend mounted below "/"
            // gets the right document base for its relative asset URLs.
            if (impl && req.method == "GET") {
                std::string loc = impl->bare_mount_redirect(impl->strip_prefix(req.path));
                if (!loc.empty()) {
                    if (!req.query.empty()) { loc += "?"; loc += req.query; }
                    session->response.status(302);
                    session->response.header("Location", loc);
                    session->response_ready = true;
                    lws_callback_on_writable(wsi);
                    return 0;
                }
            }

            // No API route. For GET, try the LocalStorage mount (which may serve
            // from outside HTTP_ROOT and enforce a presigned signature), then a
            // static file under HTTP_ROOT, before giving up with a 404.
            int rc = 0;
            if (impl && req.method == "GET") {
                if (serve_decrypted_file(wsi, impl, session, req, &rc)) return rc;
                if (serve_storage_file(wsi, impl, req, &rc)) return rc;
                if (serve_static_file(wsi, impl, req.path, &rc)) return rc;
            }
            return lws_http_transaction_completed(wsi) ? -1 :
                (lws_return_http_status(wsi, HTTP_STATUS_NOT_FOUND, "not found"), -1);
        }
        session->accept_gzip = client_accepts_gzip(req.accept_encoding);
        session->response.set_request(&req);
        (*h)(req, session->response);
        session->response_ready = true;
        if (session->response.is_async_stream()) impl->active_streams.insert(wsi);
        lws_callback_on_writable(wsi);
        return 0;
    }

    case LWS_CALLBACK_HTTP_BODY:
        if (session && in && len) {
            session->body_buf.append(static_cast<const char*>(in), len);
        }
        return 0;

    case LWS_CALLBACK_HTTP_BODY_COMPLETION: {
        if (!session || !impl) return -1;
        // Guard against a double dispatch if this fires for a request we already
        // answered at headers time (request_has_body() said no body).
        if (session->response_ready) return 0;
        Request req;
        collect_request(wsi, req);
        req.body = std::move(session->body_buf);

        auto* h = impl->match_http(req.method, req.path, req.path_params);
        if (!h) {
            lws_return_http_status(wsi, HTTP_STATUS_NOT_FOUND, "not found");
            return -1;
        }
        session->accept_gzip = client_accepts_gzip(req.accept_encoding);
        session->response.set_request(&req);
        (*h)(req, session->response);
        session->response_ready = true;
        if (session->response.is_async_stream()) impl->active_streams.insert(wsi);
        lws_callback_on_writable(wsi);
        return 0;
    }

    case LWS_CALLBACK_HTTP_WRITEABLE: {
        if (!session || !session->response_ready) return 0;

        // Async (worker-thread) streaming: bytes are pushed in from off-loop and
        // this callback is driven by EVENT_WAIT_CANCELLED wakeups.
        if (session->response.is_async_stream()) return write_async_stream(wsi, session, impl);

        // Chunked streaming takes a separate path: unknown content length, body
        // pulled from the producer one chunk at a time.
        if (session->response.is_streaming()) return write_streaming(wsi, session, impl);

        if (session->out_buf.empty() && session->out_sent == 0) {
            const std::string& body = session->response.body();

            // gzip a large text body when the client advertised it; on any zlib
            // failure fall back to the raw body. Content-Length is then the
            // *compressed* size, and Content-Encoding/Vary go out alongside.
            std::string gz;
            const bool gzipped =
                session->accept_gzip &&
                body.size() >= GZIP_MIN_BODY &&
                gzip_worthwhile_type(session->response.content_type()) &&
                gzip_compress(body, gz);

            uint8_t headers[LWS_TX_BUFFER];
            uint8_t* p = headers + LWS_PRE;
            uint8_t* end = headers + sizeof(headers);

            if (lws_add_http_common_headers(wsi,
                    static_cast<unsigned>(session->response.status()),
                    session->response.content_type().c_str(),
                    gzipped ? gz.size() : body.size(),
                    &p, end)) return -1;

            // Per-request headers plus the configured default headers (the CORS
            // Access-Control-* set) added to every response.
            if (add_extra_headers(wsi, session->response, impl, &p, end)) return -1;

            // Advertise the encoding, and that the body varies by Accept-Encoding
            // so a shared cache won't hand a gzip body to a client that can't read
            // it.
            if (gzipped) {
                if (lws_add_http_header_by_name(wsi,
                        reinterpret_cast<const unsigned char*>("content-encoding:"),
                        reinterpret_cast<const unsigned char*>("gzip"), 4, &p, end)) return -1;
                if (lws_add_http_header_by_name(wsi,
                        reinterpret_cast<const unsigned char*>("vary:"),
                        reinterpret_cast<const unsigned char*>("Accept-Encoding"),
                        15, &p, end)) return -1;
            }

            if (lws_finalize_write_http_header(wsi, headers + LWS_PRE, &p, end)) return -1;
            if (gzipped) session->out_buf = std::move(gz);
            else         session->out_buf = body;
        }

        if (session->out_sent < session->out_buf.size()) {
            size_t remaining = session->out_buf.size() - session->out_sent;
            size_t chunk = std::min<size_t>(remaining, LWS_TX_BUFFER - LWS_PRE);

            std::vector<uint8_t> buf(LWS_PRE + chunk);
            std::memcpy(buf.data() + LWS_PRE,
                        session->out_buf.data() + session->out_sent, chunk);

            int flags = (chunk == remaining) ? LWS_WRITE_HTTP_FINAL : LWS_WRITE_HTTP;
            int written = lws_write(wsi, buf.data() + LWS_PRE, chunk,
                                    static_cast<lws_write_protocol>(flags));
            if (written < 0) return -1;
            session->out_sent += written;

            if (session->out_sent < session->out_buf.size()) {
                lws_callback_on_writable(wsi);
                return 0;
            }
        }

        if (lws_http_transaction_completed(wsi)) return -1;
        return 0;
    }

    case LWS_CALLBACK_HTTP_FILE_COMPLETION:
        // A static file (served via lws_serve_http_file) finished sending. End
        // the transaction; closing if keep-alive can't continue it.
        return lws_http_transaction_completed(wsi) ? -1 : 0;

    case LWS_CALLBACK_HTTP_DROP_PROTOCOL:
        if (session) {
            // Tell any worker thread to stop producing for a connection that is
            // going away, and drop it from the wakeup set before its state dies.
            if (session->response.is_async_stream() && session->response.stream_writer()) {
                session->response.stream_writer()->cancel();
            }
            if (impl) impl->active_streams.erase(wsi);
            session->~PerHttpSession();
        }
        return 0;

    case LWS_CALLBACK_EVENT_WAIT_CANCELLED: {
        // Cross-thread wakeup from a StreamWriter (begin/write/finish). We don't
        // know which stream advanced, so re-arm every active one; each then sends
        // whatever it has buffered. Recover impl from the context when the
        // cancelled wsi has no protocol bound.
        Router::Impl* im = impl;
        if (!im) {
            lws_context* ctx = wsi ? lws_get_context(wsi) : nullptr;
            if (ctx) im = static_cast<Router::Impl*>(lws_context_user(ctx));
        }
        if (im) {
            for (auto it = im->active_streams.begin(); it != im->active_streams.end(); ++it) {
                lws_callback_on_writable(*it);
            }
        }
        return 0;
    }

    default:
        return 0;
    }
}

//------------------------------------------------------------------------------

int callback_ws(lws* wsi, enum lws_callback_reasons reason,
                void* user, void* in, size_t len) {
    auto* session = static_cast<PerWsSession*>(user);
    auto* impl = impl_of(wsi);

    switch (reason) {
    case LWS_CALLBACK_ESTABLISHED: {
        new (session) PerWsSession();
        if (!impl) { session->~PerWsSession(); return -1; }

        // Collect the upgrade request -- path/query plus the Authorization header
        // (and the usual proxy/locale headers) -- so a ws factory can authenticate
        // the handshake the same way an HTTP handler would, e.g. verify a bearer
        // JWT from the header or a ?token= query param before accepting the socket.
        Request req;
        collect_request(wsi, req);

        auto it = impl->ws_routes.find(req.path);
        if (it == impl->ws_routes.end()) { session->~PerWsSession(); return -1; }

        session->hooks = it->second(req);

        // A factory refuses the connection (e.g. failed auth) by setting
        // hooks.reject; turn that into a refused upgrade without opening.
        if (session->hooks.reject) { session->~PerWsSession(); return -1; }

        session->open = true;
        if (session->hooks.on_open) session->hooks.on_open(wsi);
        return 0;
    }

    case LWS_CALLBACK_RECEIVE: {
        if (!session || !session->open) return 0;
        bool is_binary = lws_frame_is_binary(wsi) != 0;
        if (session->hooks.on_message && in && len) {
            session->hooks.on_message(wsi,
                std::string(static_cast<const char*>(in), len), is_binary);
        }
        return 0;
    }

    case LWS_CALLBACK_SERVER_WRITEABLE: {
        if (!session || !session->open) return 0;

        std::vector<std::pair<std::string, bool>> pending;
        {
            std::lock_guard<std::mutex> lk(impl->ws_write_mutex);
            auto it = impl->external_writes.find(wsi);
            if (it != impl->external_writes.end()) {
                pending = std::move(it->second);
                impl->external_writes.erase(it);
            }
        }
        for (auto _wit = session->pending_writes.begin(); _wit != session->pending_writes.end(); ++_wit) {
            pending.emplace_back(std::move(_wit->first), _wit->second);
        }
        session->pending_writes.clear();

        for (auto _pit = pending.begin(); _pit != pending.end(); ++_pit) {
            auto& data = _pit->first;
            const bool binary = _pit->second;
            std::vector<uint8_t> buf(LWS_PRE + data.size());
            std::memcpy(buf.data() + LWS_PRE, data.data(), data.size());
            int written = lws_write(wsi, buf.data() + LWS_PRE, data.size(),
                binary ? LWS_WRITE_BINARY : LWS_WRITE_TEXT);
            if (written < static_cast<int>(data.size())) return -1;
        }
        return 0;
    }

    case LWS_CALLBACK_CLOSED:
        if (session && session->open) {
            session->open = false;
            if (session->hooks.on_close) session->hooks.on_close(wsi);
            // Drop any frames a worker queued via send_ws() that never drained
            // (the socket is gone): leaving them keyed by this wsi leaks and could
            // mis-deliver if lws later reuses the pointer for a new client. Done
            // after on_close, which flips the handler's `alive` flag so no further
            // send_ws() races in behind this erase.
            if (impl) {
                std::lock_guard<std::mutex> lk(impl->ws_write_mutex);
                impl->external_writes.erase(wsi);
            }
            session->~PerWsSession();
        }
        return 0;

    default:
        return 0;
    }
}

}

//------------------------------------------------------------------------------
// Router public interface
//------------------------------------------------------------------------------

Router::Router() : impl_(std::unique_ptr<Impl>(new Impl())) {
    impl_->self = this;
}

Router::~Router() = default;

void Router::get(std::string path, HttpHandler h)  { impl_->add_route("GET",    path, std::move(h)); }
void Router::get_unprefixed(std::string path, HttpHandler h) {
    impl_->add_route("GET", path, std::move(h), /*apply_prefix=*/false);
}
void Router::post(std::string path, HttpHandler h) { impl_->add_route("POST",   path, std::move(h)); }
void Router::put(std::string path, HttpHandler h)  { impl_->add_route("PUT",    path, std::move(h)); }
void Router::del(std::string path, HttpHandler h)  { impl_->add_route("DELETE", path, std::move(h)); }

void Router::http(std::string path, HttpHandler h, unsigned methods) {
    // Route through add_route so the uri_prefix and the parameterized-segment
    // handling apply uniformly, rather than poking http_routes directly.
    if (methods & GET)  impl_->add_route("GET",    path, h);
    if (methods & POST) impl_->add_route("POST",   path, h);
    if (methods & PUT)  impl_->add_route("PUT",    path, h);
    if (methods & DEL)  impl_->add_route("DELETE", path, std::move(h));
}

void Router::ws(std::string path, WsHandlerFactory f) {
    impl_->ws_routes[impl_->with_prefix(path)] = std::move(f);
}

void Router::set_default_headers(std::unordered_map<std::string, std::string> headers) {
    impl_->default_headers.assign(headers.begin(), headers.end());
}

void Router::set_http_roots(std::vector<std::pair<std::string, std::string>> roots) {
    // Mount-paths arrive already normalized and sorted most-specific-first (see
    // Config::http_roots); store them verbatim.
    impl_->http_roots = std::move(roots);
}

void Router::set_storage_mount(std::string url_prefix, std::string dir, std::string secret) {
    // Normalize url_prefix to a single leading '/' and no trailing '/', so the
    // "<mount>/..." prefix test in serve_storage_file holds. An empty path (a
    // bare-origin base URL) leaves the mount disabled.
    while (!url_prefix.empty() && url_prefix.back() == '/') url_prefix.pop_back();
    if (!url_prefix.empty() && url_prefix.front() != '/') url_prefix.insert(url_prefix.begin(), '/');
    impl_->storage_mount  = std::move(url_prefix);
    impl_->storage_dir    = std::move(dir);
    impl_->storage_secret = std::move(secret);
}

void Router::set_file_mount(std::string url_prefix, storage::Storage* storage, std::string secret) {
    // Same normalization as set_storage_mount: a single leading '/', no trailing
    // '/', so the "<mount>/..." prefix test in serve_decrypted_file holds.
    while (!url_prefix.empty() && url_prefix.back() == '/') url_prefix.pop_back();
    if (!url_prefix.empty() && url_prefix.front() != '/') url_prefix.insert(url_prefix.begin(), '/');
    impl_->file_mount   = std::move(url_prefix);
    impl_->file_storage = storage;
    impl_->file_secret  = std::move(secret);
}

void Router::set_uri_prefix(std::string prefix) {
    impl_->uri_prefix = std::move(prefix);
}

void Router::apply_to(lws_context_creation_info& info) {
    impl_->protocols.clear();
    impl_->protocols.push_back({
        "http", callback_http, sizeof(PerHttpSession), 0, 0, impl_.get(), 0
    });
    impl_->protocols.push_back({
        "ws", callback_ws, sizeof(PerWsSession), 64 * 1024, 0, impl_.get(), 0
    });
    impl_->protocols.push_back({nullptr, nullptr, 0, 0, 0, nullptr, 0});
    info.protocols = impl_->protocols.data();
    // Recoverable from any callback via lws_context_user(), including the
    // EVENT_WAIT_CANCELLED wakeup where the wsi has no protocol bound.
    info.user = impl_.get();
}

void Router::send_ws(lws* wsi, const std::string& payload, bool is_binary) {
    {
        std::lock_guard<std::mutex> lk(impl_->ws_write_mutex);
        impl_->external_writes[wsi].emplace_back(payload, is_binary);
    }
    lws_callback_on_writable(wsi);
}

}
}

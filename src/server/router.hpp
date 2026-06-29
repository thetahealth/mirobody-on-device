#pragma once

#include <functional>
#include <memory>
#include <cstdint>
#include <string>
#include "compat/cxx11.hpp"
#include <unordered_map>
#include <utility>
#include <vector>

#include "server/request.hpp"    // Request
#include "server/response.hpp"   // StreamWriter, SseEvent, Response

struct lws;
struct lws_context_creation_info;

namespace mirobody { namespace storage { class Storage; } }

namespace mirobody { namespace server {

using HttpHandler = std::function<void(const Request&, Response&)>;

// HTTP method bitmask for Router::http(), combinable with `|`, e.g.
// `router.http("/x", h, GET | POST)`. (DEL, not DELETE, since DELETE is a
// Windows macro.)
enum Method : unsigned {
    GET  = 1u << 0,
    POST = 1u << 1,
    PUT  = 1u << 2,
    DEL  = 1u << 3,
};

//------------------------------------------------------------------------------
// WebSocket handler hooks
//------------------------------------------------------------------------------

struct WsHandlerHooks {
    std::function<void(lws*)> on_open;
    std::function<void(lws*, const std::string& payload, bool is_binary)> on_message;
    std::function<void(lws*)> on_close;

    // Set by a factory to refuse the handshake (e.g. failed auth): the router
    // turns it into a rejected upgrade and never calls on_open/on_message. The
    // factory receives the upgrade Request (path, query, Authorization header)
    // so it can decide before the socket is accepted.
    bool reject = false;
};
using WsHandlerFactory = std::function<WsHandlerHooks(const Request&)>;

//------------------------------------------------------------------------------
// Router
//------------------------------------------------------------------------------

class Router {
public:
    Router();
    ~Router();

    Router(const Router&) = delete;
    Router& operator=(const Router&) = delete;

    void get(std::string path, HttpHandler h);
    void post(std::string path, HttpHandler h);
    void put(std::string path, HttpHandler h);
    void del(std::string path, HttpHandler h);

    // Like get(), but registers `path` verbatim, exempt from the uri_prefix, so
    // it stays reachable at its bare absolute path regardless of where the app is
    // mounted. For infrastructure endpoints addressed without prefix knowledge --
    // notably GET /api/health, which load-balancer / k8s probes hit directly.
    void get_unprefixed(std::string path, HttpHandler h);

    // Register `h` for every method set in the `methods` bitmask, e.g.
    // `router.http("/x", h, GET | POST)`.
    void http(std::string path, HttpHandler h, unsigned methods);

    void ws(std::string path, WsHandlerFactory factory);

    // Response headers added to every response (the HTTP_HEADERS config map,
    // typically the Access-Control-* CORS set). When non-empty, OPTIONS
    // requests are also answered centrally as CORS preflight (204 + these
    // headers). Call before serving; default is none.
    void set_default_headers(std::unordered_map<std::string, std::string> headers);

    // Directories served as static content for GET requests that match no
    // registered route (the HTTP_ROOT config value). Each entry is
    // {mount-path, directory}: a request under a mount-path is served from that
    // directory, with the mount-path stripped first, so several frontends can
    // be hosted at distinct URL prefixes (e.g. {"/", "res/htdoc"} plus
    // {"/dashboard", "res/dashboard"}). A request for a mount root or any path
    // ending in "/" serves its "index.html". Mounts are matched
    // most-specific-prefix-first, so a deeper mount wins over the "/" catch-all.
    // An empty list (the default) disables static serving, so unmatched GETs 404
    // as before.
    void set_http_roots(std::vector<std::pair<std::string, std::string>> roots);

    // Serve LocalStorage objects over HTTP. Unmatched GETs whose path is under
    // `url_prefix` are served from `dir` -- which, unlike the HTTP_ROOT static
    // dir, may live OUTSIDE the web root, so user uploads aren't exposed as
    // ordinary static assets. `url_prefix` is a URL path only: it is stripped to
    // recover the object key, which maps directly under `dir` (no on-disk
    // prefix). When `secret` is non-empty, each request must carry a valid
    // "?expires=&sig=" HMAC minted by LocalStorage::presigned_url (correct
    // signature and unexpired) or it is rejected with 403; an empty secret
    // serves unconditionally. `url_prefix` is LOCAL_STORAGE_URL_PREFIX
    // (normalized to a leading '/', no trailing '/'); `dir` is LOCAL_STORAGE_DIR;
    // `secret` is LOCAL_STORAGE_SECRET. An empty `url_prefix` or `dir` disables
    // the mount. The mount is checked before the HTTP_ROOT static fallback.
    void set_storage_mount(std::string url_prefix, std::string dir, std::string secret);

    // Serve encrypted user uploads through this server, decrypting them in
    // memory. When FILE_ENCRYPTION_KEY is configured, upload bytes live in
    // object storage as ciphertext that a client cannot decrypt, so the
    // bucket-direct / LocalStorage-disk mounts can't serve them; this mount
    // does. Unmatched GETs under `url_prefix` are treated as a full object key
    // (url_prefix stripped, percent-decoded); the request must carry a valid
    // "?expires=&sig=" HMAC minted by Storage::signed_read_url (keyed by
    // `secret`, unexpired) or it is rejected with 403. On success the bytes are
    // fetched via `storage->get_object_decrypted()` and returned with a
    // Content-Type inferred from the key (text/plain for a .trans transcript).
    // `storage` is borrowed and must outlive the router. An empty `url_prefix`
    // or null `storage` disables the mount. Checked before the LocalStorage
    // disk mount and the static fallback.
    void set_file_mount(std::string url_prefix, storage::Storage* storage, std::string secret);

    // URI prefix prepended to every route registered afterwards (HTTP and
    // WebSocket) and stripped from request paths before static files are
    // resolved, so the whole app can live under a sub-path (e.g. "/mirobody")
    // behind an ingress that does not strip it. Must be called BEFORE any
    // get/post/put/del/http/ws registration, since the prefix is baked into the
    // route key at registration time. Pass a normalized value: a leading slash
    // and no trailing slash (e.g. "/mirobody"). Empty (the default) registers
    // every route at the root, exactly as before.
    void set_uri_prefix(std::string prefix);

    void apply_to(lws_context_creation_info& info);

    void send_ws(lws* wsi, const std::string& payload, bool is_binary);

    struct Impl;

private:
    std::unique_ptr<Impl> impl_;
};

}
}

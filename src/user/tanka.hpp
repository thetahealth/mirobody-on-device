#pragma once

#include "config/config.hpp"
#include "server/router.hpp"   // Router, Request, Response

#include <condition_variable>
#include <functional>
#include <mutex>
#include <string>
#include <thread>

namespace mirobody { namespace user {

// Tanka QR-code sign-in (web), split out of UserService. The browser runs Tanka's
// own WASM request-signer and POSTs already-signed {headers, body} envelopes to
// /tanka/qrcode (create the login QR) and /tanka/poll (check for a confirmed
// scan); both PROXY the signed request to Tanka's gateway server-side (the browser
// can't -- CORS). GET /tanka/verify advertises whether the feature is enabled (so
// the web client shows/hides the button). GET /tanka-sign.wasm bridges Tanka's
// signing WASM and GET /tanka-signer.js serves the matching wasm-bindgen glue.
//
// On a confirmed scan the email is resolved from Tanka's response and handed to
// the `on_login` callback supplied at construction, which creates-or-gets the user
// and writes the standard auth envelope -- this is the only coupling back to the
// user domain, so TankaService owns no database/JWT state itself.
//
// Self-healing: when enabled and TANKA_WASM_AUTODISCOVER is on, a background thread
// runs the Node discovery engine (res/tanka/discover.cjs) at boot and every
// TANKA_WASM_REFRESH_INTERVAL seconds. It finds Tanka's live signer build and
// VERIFIES it (signs a real create-QR; Tanka must answer code:0), then adopts it
// in memory -- swapping the served glue + bridged WASM in lockstep. Requires
// `node` on PATH; degrades to the pinned fallback when node/Tanka are absent.
//
// Lifetime: borrows `router` (registers its routes) and copies what it needs from
// `cfg`; the Router must outlive the running server. The destructor stops + joins
// the auto-discovery thread.
class TankaService {
public:
    // Invoked on a confirmed scan with the resolved email; must create-or-get the
    // user and write the standard auth envelope onto the Response.
    using LoginFn = std::function<void(const server::Request&, server::Response&,
                                       const std::string& email)>;

    TankaService(server::Router& router, const Config& cfg, LoginFn on_login);

    TankaService(const TankaService&)            = delete;
    TankaService& operator=(const TankaService&) = delete;

    ~TankaService();

private:
    void register_routes(server::Router& router);

    void on_verify(const server::Request& req, server::Response& res);
    void on_qrcode(const server::Request& req, server::Response& res);
    void on_poll(const server::Request& req, server::Response& res);
    void on_wasm(const server::Request& req, server::Response& res);
    void on_signer_js(const server::Request& req, server::Response& res);

    // The signing-WASM URL in effect: the auto-discovered build when adopted, else
    // web_origin_ + the pinned path constant. Caller must hold wasm_mu_.
    std::string effective_wasm_url() const;
    // Lazily fetch + cache the effective WASM (cache keyed by URL so an adoption
    // forces a refetch). nullptr when it can't be obtained. Thread-safe.
    const std::string* load_wasm();
    // The glue to serve: the adopted build's when present, else the vendored
    // fallback (read once). nullptr if neither is available. Thread-safe.
    const std::string* signer_js();

    void start_autodiscovery();
    void refresh_loop();
    bool refresh_once();

    LoginFn on_login_;

    bool        enabled_ = false;
    std::string api_base_;
    std::string create_path_;
    std::string check_path_;
    std::string web_origin_;   // Origin pinned on Tanka calls + WASM host
    int         success_code_ = 0;
    int         http_timeout_ = 15;

    bool        autodiscover_ = true;
    int         refresh_interval_ = 7 * 24 * 3600;
    std::string node_bin_;
    std::string discover_script_;
    std::string signer_js_path_;   // vendored fallback glue

    // WASM + served-glue state, guarded by wasm_mu_ (see service members' notes).
    std::mutex  wasm_mu_;
    std::string wasm_bytes_;
    std::string wasm_bytes_url_;
    std::string adopted_wasm_url_;
    std::string adopted_glue_;
    std::string fallback_glue_;
    bool        fallback_glue_loaded_ = false;

    // Background auto-discovery thread + its stop signal.
    std::thread             refresh_thread_;
    std::mutex              refresh_mu_;
    std::condition_variable refresh_cv_;
    bool                    stopping_ = false;
};

}}

#pragma once

#include "cache/cache.hpp"
#include "config/config.hpp"
#include "database/database.hpp"
#include "database/enums.hpp"   // LoginMethod
#include "jwt/apple.hpp"
#include "jwt/firebase.hpp"
#include "jwt/jwt.hpp"
#include "server/router.hpp"
#include "user/email.hpp"
#include "user/tanka.hpp"

#include <memory>

namespace mirobody { namespace user {

// Groups the user-domain HTTP routes behind a single object. Constructing the
// service wires every route it owns onto the supplied Router, so callers only
// need to instantiate it once at startup:
//
//     user::UserService user_svc(router, db, cache, jwt, firebase);
//
// The service *borrows* `cfg`, `db`, `cache`, and `jwt` (it does not own them)
// and keeps them for its route handlers to use — `jwt` mints/verifies the
// tokens its endpoints issue, and the email validator (built from `cfg` and
// backed by `cache`) sends/checks login verification codes. `cache` is taken by
// non-const reference because the email validator mutates it (stored codes and
// send cooldowns). `firebase` validates Firebase Auth ID tokens for the
// sign-in/exchange flow and is OPTIONAL: it is null when firebase_project_id
// is unset, so handlers must check before using it. `apple` validates Apple
// "Sign in with Apple" ID tokens and is likewise OPTIONAL (null when
// apple_client_id is unset). Lifetime contract: every borrowed object and the
// Router (and therefore this service, since its handlers capture it) must
// outlive the running server.
class UserService {
public:
    UserService(server::Router& router,
                const Config& cfg,
                database::Database& db,
                cache::Cache& cache,
                const jwt::Jwt& jwt,
                jwt::FirebaseTokenValidator* firebase,
                jwt::AppleTokenValidator* apple = nullptr);

    UserService(const UserService&)            = delete;
    UserService& operator=(const UserService&) = delete;

private:
    // Registers all user-domain routes onto `router` (thin wiring to the
    // per-route handlers below). Called from the ctor.
    void register_routes(server::Router& router);

    // Route handlers, one per endpoint. Each takes the request and writes the
    // response; register_routes() forwards to them. See the .cpp for the
    // request/response contract documented above each.
    void on_email_login(const server::Request& req, server::Response& res);
    void on_email_verify(const server::Request& req, server::Response& res);
    void on_email_bind(const server::Request& req, server::Response& res);
    void on_email_bind_verify(const server::Request& req, server::Response& res);
    void on_firebase_verify(const server::Request& req, server::Response& res);
    void on_apple_verify(const server::Request& req, server::Response& res);
    void on_wechat_verify(const server::Request& req, server::Response& res);
    void on_github_verify(const server::Request& req, server::Response& res);
    // GET /auth/providers -- which federated sign-ins this deployment has
    // configured, plus each one's public config, in a single public document.
    void on_auth_providers(const server::Request& req, server::Response& res);

    // Confirmed-scan callback for TankaService: create-or-get the user for `email`
    // and write the standard auth envelope (the Tanka login flow lives in
    // src/user/tanka.{hpp,cpp}; this is its only coupling back to the user domain).
    void tanka_login(const server::Request& req, server::Response& res,
                     const std::string& email);

    // Look up the user with `email`, creating the row if absent. Returns the
    // user id (> 0) on success; on failure returns 0 and writes the reason to
    // *err. Mirrors the Python add_or_get_user (email-only path).
    std::int64_t add_or_get_user(const std::string& email, std::string* err);

    // Look up the WeChat user with `openid`, creating the row if absent. The
    // lookup key is the wechat_openid column (the stable per-app identifier);
    // a created row also gets a synthetic "<unionid|openid>@wechat" address so
    // the NOT NULL + unique email column is satisfied for accounts with no real
    // email. `unionid` may be empty. Returns the user id (> 0) on success; on
    // failure returns 0 and writes the reason to *err.
    std::int64_t add_or_get_wechat_user(const std::string& openid,
                                        const std::string& unionid,
                                        std::string* err);

    // Resolve a third-party login that has no *verified* email by its provider
    // identity instead: look up user_identities by (login_method, provider_uid),
    // creating a users row + identity row (in one transaction) when absent. The
    // provider-reported `email` (which may be empty or unverified) is stored on
    // the identity row only -- never promoted to users.email. Returns the user id
    // (> 0) on success; 0 + *err on failure. New-schema only (the caller gates
    // this behind MIROBODY_DATABASE_PG_LEGACY).
    std::int64_t add_or_get_identity_user(int login_method,
                                          const std::string& provider_uid,
                                          const std::string& email,
                                          std::string* err);

    // Set `email` as the (verified) primary email of the logged-in user `user_id`.
    // Caller must have verified ownership (OTP) first. Rejects an address already
    // held by another active account -- there is no auto-merge. Returns true on
    // success; false + *err otherwise. Used by /email/bind/verify.
    bool bind_email(std::int64_t user_id, const std::string& email, std::string* err);

    // Mint access + refresh tokens for (user_id, email) and write the standard
    // auth envelope onto `res`. Mirrors the Python _generate_auth_response
    // (sans the WebAuthn MFA branch, which this build does not implement).
    void generate_auth_response(server::Response& res,
                                std::int64_t user_id, const std::string& email);

    // Append a user_login_logs row for a successful login as `user_id` via
    // `method`. Best-effort (a failed write is logged and swallowed); `req`
    // supplies the client ip / user-agent. No-op on the legacy backend.
    void log_login(const server::Request& req, std::int64_t user_id,
                   database::LoginMethod method);

    database::Database&          db_;
    cache::Cache&                cache_;
    const jwt::Jwt&              jwt_;
    jwt::FirebaseTokenValidator* firebase_;   // borrowed; null when unconfigured
    jwt::AppleTokenValidator*    apple_;      // borrowed; null when unconfigured

    // Sends and verifies email verification codes. Always non-null (the factory
    // returns a Dummy validator when no transport is configured).
    std::unique_ptr<EmailCodeValidator> email_validator_;

    // Firebase web-app config JSON, served by GET /firebase/verify and included in
    // GET /auth/providers, so a client can initialize the Firebase JS SDK. Built
    // once from `cfg` in the ctor; empty when unconfigured.
    std::string firebase_web_config_;

    // Apple web config JSON ({"clientId": ...}), served by GET /apple/verify and
    // included in GET /auth/providers, so a client can initialize the Apple JS SDK.
    // Built once from `cfg` in the ctor; empty when unconfigured (no APPLE_CLIENT_ID).
    std::string apple_web_config_;

    // WeChat Mini Program credentials for POST /wechat/verify, copied from `cfg`
    // in the ctor. Sign-in is enabled only when both appid and secret are set.
    // api_base is the jscode2session host (overridable for tests).
    std::string wechat_appid_;
    std::string wechat_secret_;
    std::string wechat_api_base_;

    // WeChat *mobile app* credentials for the native Android / iOS apps (POST
    // /wechat/verify with {"flow":"app"}, exchanged via sns/oauth2/access_token).
    // Fall back to the web, then the Mini Program credentials when unset.
    std::string wechat_app_appid_;
    std::string wechat_app_secret_;

    // WeChat *web* credentials for the browser sign-in flows (POST /wechat/verify
    // with {"flow":"web"}, exchanged via sns/oauth2/access_token). Fall back to
    // the Mini Program credentials above when no separate web creds are set.
    // wechat_web_config_ is the JSON {"appid":...} served by GET /wechat/verify
    // (and included in GET /auth/providers)
    // so the web client can build the authorize URL; empty when unconfigured.
    std::string wechat_web_appid_;
    std::string wechat_web_secret_;
    std::string wechat_web_config_;

    // GitHub OAuth credentials for POST /github/verify, copied from `cfg` in the
    // ctor. Sign-in is enabled only when both client_id and client_secret are
    // set. oauth_base is the authorize/token host (github.com) and api_base is
    // the REST API host (api.github.com); both are overridable for tests.
    // github_web_config_ is the JSON {"clientId":...} served by GET
    // /github/verify so the web client can build the authorize URL; empty when
    // unconfigured.
    std::string github_client_id_;
    std::string github_client_secret_;
    std::string github_oauth_base_;
    std::string github_api_base_;
    std::string github_web_config_;

    // The whole sign-in capability set as one JSON object, served by
    // GET /auth/providers so a client can render its sign-in screen from a single
    // request instead of probing the four per-provider GET routes. Composed in the
    // ctor from the config strings above, so it must be declared AFTER them.
    std::string auth_providers_;

    // Tanka QR-code sign-in, owned here so its routes + auto-discovery thread share
    // this service's lifetime. It calls tanka_login() (above) on a confirmed scan;
    // all its other state lives in src/user/tanka.{hpp,cpp}. Declared last so it is
    // destroyed first (its dtor joins the discovery thread before our members go).
    std::unique_ptr<TankaService> tanka_;
};

}}

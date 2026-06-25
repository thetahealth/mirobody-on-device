#pragma once

#include "cache/cache.hpp"
#include "config/config.hpp"
#include "jwt/jwt.hpp"
#include "server/router.hpp"

#include <string>

namespace mirobody { namespace oauth {

// OAuth 2.0 *authorization server* for mirobody. Lets MCP clients (and any
// standard OAuth client) obtain access tokens for this server through the
// browser authorization-code + PKCE flow, then present them as bearer tokens to
// the protected MCP endpoint. The tokens it issues are the very same mb_oauth
// HS256 JWTs the rest of the system mints and verifies (see jwt::Jwt) — this
// class only adds the OAuth protocol surface around them:
//
//   GET  /.well-known/oauth-protected-resource    (RFC 9728)
//   GET  /.well-known/oauth-authorization-server  (RFC 8414)
//   POST /oauth/register                          (RFC 7591 dynamic registration)
//   GET  /oauth/authorize                         (RFC 6749 + PKCE S256)
//   GET  /oauth/authorize/info                    (consent-card metadata; web UI)
//   POST /oauth/authorize/decision                (user approves; mints the code)
//   POST /oauth/token                             (authorization_code + refresh_token)
//   POST /oauth/revoke                            (RFC 7009, best-effort)
//
// The discovery documents are served at the host root (unprefixed) so clients
// find them regardless of HTTP_URI_PREFIX; the endpoints they advertise carry
// the prefix. End-user login + consent reuse the existing web client: the
// authorize endpoint redirects the browser to the SPA (OAUTH_CONSENT_PATH) with
// a one-time request handle, the SPA signs the user in with the providers it
// already supports, then POSTs the decision back with the user's bearer token.
//
// State (registered clients, pending authorize requests, single-use codes) lives
// in `cache` only; refresh tokens are stateless JWTs. The service *borrows*
// `cfg`, `cache`, and `jwt` — they must outlive it (and the Router it installs
// onto). Constructing it wires every route onto `router`.
class OAuthService {
public:
    // `jwks_json` is the public JWKS document to publish at
    // /.well-known/jwks.json (and advertise as `jwks_uri` in discovery) so
    // third-party resource servers can verify the tokens this server issues.
    // Empty under HS256 (no public key) — the endpoint and jwks_uri are then
    // omitted.
    OAuthService(server::Router& router,
                 const Config& cfg,
                 cache::Cache& cache,
                 const jwt::Jwt& jwt,
                 std::string jwks_json = std::string());

    OAuthService(const OAuthService&)            = delete;
    OAuthService& operator=(const OAuthService&) = delete;

private:
    void register_routes(server::Router& router);

    // Discovery.
    void protected_resource_metadata(const server::Request& req, server::Response& res);
    void authorization_server_metadata(const server::Request& req, server::Response& res);

    // Client + flow endpoints.
    void register_client(const server::Request& req, server::Response& res);
    void authorize(const server::Request& req, server::Response& res);
    void authorize_info(const server::Request& req, server::Response& res);
    void authorize_decision(const server::Request& req, server::Response& res);
    void token(const server::Request& req, server::Response& res);
    void revoke(const server::Request& req, server::Response& res);

    // External base URL (scheme://host) for this request: OAUTH_ISSUER when
    // configured, else derived from the Host header (http for loopback, https
    // otherwise). This is the discovery `issuer`.
    std::string base_url(const server::Request& req) const;
    // base_url + uri_prefix + `path` — an absolute endpoint URL clients can call.
    std::string endpoint(const server::Request& req, const std::string& path) const;

    // Whether `uri` is an acceptable redirect target: loopback http, any custom
    // scheme, or https (restricted to OAUTH_ALLOWED_REDIRECT_HOSTS when set).
    bool redirect_uri_allowed(const std::string& uri) const;

    const Config&   cfg_;
    cache::Cache&   cache_;
    const jwt::Jwt& jwt_;
    std::string     uri_prefix_;   // copied from cfg_.uri_prefix at construction
    std::string     jwks_json_;    // public JWKS to serve; empty under HS256
};

}}

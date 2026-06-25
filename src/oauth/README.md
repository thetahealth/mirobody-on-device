# src/oauth — OAuth 2.0 authorization server

An OAuth 2.0 **authorization server** embedded in the mirobody core. It lets
third-party OAuth clients — primarily **MCP clients** (Claude, IDEs, …) — obtain
access tokens for this server through the browser **authorization-code + PKCE**
flow, then present them as bearer tokens to the protected MCP endpoint.

The tokens it issues are the *same* `mb_oauth` HS256 JWTs the rest of the system
already mints and verifies (see [src/jwt/](../jwt/)); this module only adds the
OAuth protocol surface around them. The MCP endpoint ([src/mcp/](../mcp/))
already validates those bearer tokens — what was missing was a standards-
compliant way for a client to *get* one without a human pasting a token. That is
what this provides.

End-user login and consent **reuse the existing web client** ([htdoc/](../../htdoc/)):
the authorize endpoint redirects the browser to the SPA, which signs the user in
with the providers it already supports (email / Google / Apple / GitHub / WeChat)
and then posts the consent decision back.

## Module layout

```
src/oauth/
  pkce.hpp / .cpp     # base64url, SHA-256, PKCE S256 verify, CSPRNG random_token
  service.hpp / .cpp  # OAuthService: registers the OAuth routes on the Router
```

`OAuthService` is constructed in [src/server/server.cpp](../server/server.cpp) alongside the
other domain services and self-registers its routes on the shared `Router`. It
*borrows* the `Config`, the `cache::Cache`, and the `jwt::Jwt` — no database is
involved (state lives in the cache; tokens are stateless JWTs).

## Endpoints

Discovery is served at the **host root** (unprefixed) so clients find it
regardless of `HTTP_URI_PREFIX`; every other endpoint is mounted under the
prefix, and the absolute URLs inside the discovery documents carry it.

| Method | Path | RFC | Purpose |
| ------ | ---- | --- | ------- |
| GET  | `/.well-known/oauth-protected-resource`   | 9728 | resource id + which AS guards it |
| GET  | `/.well-known/oauth-authorization-server` | 8414 | endpoint + capability metadata |
| GET  | `/.well-known/openid-configuration`       | OIDC | compatibility alias → same 8414 body |
| GET  | `/.well-known/jwks.json`                  | 7517 | public JWKS (RS256 only; for third-party token validators) |
| POST | `/oauth/register`            | 7591 | dynamic client registration (public client) |
| GET  | `/oauth/authorize`           | 6749 | validate request, redirect to web consent |
| GET  | `/oauth/authorize/info`      | —    | consent-card metadata for the web UI |
| POST | `/oauth/authorize/decision`  | —    | user approves/denies → mints the code |
| POST | `/oauth/token`               | 6749 | `authorization_code` + `refresh_token` grants |
| POST | `/oauth/revoke`              | 7009 | best-effort revocation |

The discovery, `register`, `authorize`, `token`, and `revoke` endpoints return
**raw RFC-shaped JSON** (OAuth clients expect exact spec bodies, e.g.
`{"error":"invalid_grant"}` with HTTP 400). The two `authorize/*` helper
endpoints used only by the web UI return the project `{code,msg,data}` envelope,
since the SPA's `net.get`/`net.post` consume that.

## The flow

```
MCP client                     mirobody                         web client (SPA)
   │ POST /mcp (no token)          │                                  │
   │◀──── 401 + WWW-Authenticate ──│  resource_metadata="…/.well-known/oauth-protected-resource"
   │ GET  /.well-known/* ─────────▶│  discovery (issuer, endpoints, S256)
   │ POST /oauth/register ────────▶│  → client_id (PKCE public client)
   │ open browser:                 │                                  │
   │ GET /oauth/authorize?… ──────▶│  validate client/redirect/PKCE   │
   │                               │── 302 → /?oauth_consent=<h> ────▶│ sign in (reuse login)
   │                               │◀── GET /oauth/authorize/info ────│ render consent card
   │                               │◀── POST /oauth/authorize/decision│ (bearer JWT) approve
   │◀── 302 redirect_uri?code=… ───┼─────────────────────────────────│
   │ POST /oauth/token            ▶│  verify PKCE + single-use code   │
   │◀──── access + refresh JWT ────│                                  │
   │ POST /mcp (Bearer access) ───▶│  McpService verifies the JWT  ✔  │
```

The 401 challenge that kicks this off is emitted by `McpService` (see
[src/mcp/service.cpp](../mcp/service.cpp)) when an auth-flagged tool is called
without a valid token.

## State (cache only)

No schema migration — everything lives in `cache::Cache` (Redis in prod, the
in-process store in dev), keyed by prefix:

| Key | Value | TTL | Notes |
| --- | ----- | --- | ----- |
| `oauth:client:<id>`    | client metadata JSON      | `OAUTH_CLIENT_TTL` (1 yr) | registered clients |
| `oauth:authzreq:<h>`   | pending authorize request | `OAUTH_AUTHZ_TTL` (10 m)  | consumed on decision |
| `oauth:code:<code>`    | grant (user, PKCE, scope) | `OAUTH_CODE_TTL` (10 m)   | **single-use** — deleted on exchange |

Refresh tokens are **stateless JWTs** (claim `token_type: oauth_refresh_token`,
2× the access lifetime), so there is nothing server-side to store or look up for
them. Consequently `revoke` can drop a cached authorization code but cannot
truly invalidate an outstanding refresh/access JWT before its `exp` — per RFC
7009 it returns 200 regardless.

## Security model

- **PKCE S256 is mandatory.** `code_challenge_method=plain` and missing
  challenges are rejected; the verifier is checked at the token endpoint with a
  constant-time compare ([pkce.cpp](pkce.cpp)).
- **Public clients only** — no client secrets are issued
  (`token_endpoint_auth_method: none`). This is what MCP/native clients use and
  the most secure default; PKCE binds the code to the client.
- **Authorization codes are single-use** and consumed *before* validation, so a
  replay (even concurrent) cannot find the code a second time.
- **Redirect-URI policy** (`redirect_uri_allowed`): loopback `http://127.0.0.1`
  / `localhost` / `[::1]` (RFC 8252 native apps), any `https://` URI, and custom
  schemes (`myapp://…`); plaintext non-loopback `http://` is always rejected. An
  invalid `client_id`/`redirect_uri` is reported directly (never by redirecting
  to an unvalidated URI); other protocol errors redirect back with `?error=`.
- Set `OAUTH_ALLOWED_REDIRECT_HOSTS` to additionally restrict `https` redirects
  to a host allowlist.

## Token signing & JWKS

The tokens are signed by the shared [jwt::Jwt](../jwt/jwt.hpp), configured at
startup ([src/server/server.cpp](../server/server.cpp)):

- **HS256 (default).** Symmetric, keyed by `JWT_KEY`. The same process issues and
  verifies, so a shared secret is enough. No public key exists, so **no JWKS** is
  published and discovery advertises no `jwks_uri`. This is the right choice when
  only mirobody validates its own tokens (the MCP endpoint).
- **RS256.** Enabled by setting `JWT_PRIVATE_KEY` (PEM), which **takes precedence
  over `JWT_KEY`** (HS256) — when it is set, `JWT_KEY` is ignored (the server
  logs a warning if both are present). The server then signs with the private
  key and **publishes the public key** as a JWKS at
  `/.well-known/jwks.json`, advertised as `jwks_uri` in the 8414 / openid
  metadata — so a *separate* resource server can verify mirobody-issued tokens
  without holding any secret. `JWT_PUBLIC_KEY` is optional (derived from the
  private key when omitted).

Each RS256 token header carries a `kid` equal to the key's **RFC 7638 JWK
thumbprint**, and the published JWK uses the same `kid`, so a validator selects
the right key. The thumbprint + JWK derivation live in [jwt.cpp](../jwt/jwt.cpp)
(`JwtRs256::jwks_json()` / `kid()`).

**Key rotation.** Add verify-only public keys via the suffixed
`JWT_PUBLIC_KEY_2`, `JWT_PUBLIC_KEY_3`, … (scanned contiguously). They are
published in the JWKS *and* accepted on verification, so tokens signed by a
previous key keep validating while it is rotated out — the active signer remains
the single `JWT_PRIVATE_KEY`. Zero-downtime rotation: promote the new key to
`JWT_PRIVATE_KEY`, move the old public to `JWT_PUBLIC_KEY_2`, then drop it once
tokens signed by it have expired. (Our own verification tries every configured
key; third-party validators use the `kid`.)

## Configuration

All keys are optional with working defaults — OAuth works out of the box once
`JWT_KEY` is set (see [config.yml](../../config.yml), `OAuthConfig` in
[src/config/config.hpp](../config/config.hpp)).

| Key | Default | Meaning |
| --- | ------- | ------- |
| `OAUTH_ISSUER`         | *(derived from Host)* | public base URL in discovery / endpoint URLs |
| `OAUTH_CONSENT_PATH`   | `/`                   | SPA path the authorize endpoint redirects to |
| `OAUTH_CODE_TTL`       | `600`                 | authorization-code lifetime (s) |
| `OAUTH_AUTHZ_TTL`      | `600`                 | pending-request lifetime (s) |
| `OAUTH_CLIENT_TTL`     | `31536000`            | registered-client lifetime (s) |
| `OAUTH_SCOPES`         | `mcp:read mcp:write`  | scopes advertised + default-granted |
| `OAUTH_RESOURCE_PATH`  | `/mcp`                | protected resource the challenge points at |
| `OAUTH_ALLOWED_REDIRECT_HOSTS` | *(any https)* | optional https redirect host allowlist |

When `OAUTH_ISSUER` is unset the issuer is derived per-request from the `Host`
header (`http` for `localhost`/`127.0.0.1`, `https` otherwise) — correct for
single-origin deployments and dev. Set it when behind a proxy whose external
origin differs from the `Host` it forwards.

## Web consent

[htdoc/src/consent.js](../../htdoc/src/consent.js) handles the `?oauth_consent=`
load: when no token is held the normal login view runs and consent resumes after
`app.completeLogin` re-renders; with a token it fetches `/oauth/authorize/info`,
renders the consent card, and POSTs the decision. The hook lives in
[htdoc/src/app.js](../../htdoc/src/app.js) `render()`.

## Testing

[tests/oauth/pkce_test.cpp](../../tests/oauth/pkce_test.cpp) covers the security-
critical encoding/crypto layer: the RFC 7636 Appendix B PKCE vector, base64url
round-trips across every length mod 3, decode rejection of invalid input, and
`random_token` shape/uniqueness. The route handlers dispatch through
libwebsockets, so they are exercised end-to-end against a running server (e.g.
via `/verify`) rather than as unit tests, matching the rest of the tree.

```
build\tests\mirobody_tests.exe "[oauth]"
```

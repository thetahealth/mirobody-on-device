# `src/user` — the user / authentication service

> **On a phone this layer is transitional.** The native apps sign in to the
> in-process core over loopback today, so it stays until they authenticate with a
> per-launch token instead; then accounts belong to the mirobody server the app
> connects to, and this module leaves with the care-circle and OAuth layers.

The user service owns sign-in. Every flow ends the same way — resolve the caller
to a row in `health_app_user` (creating it on first sight) and return a standard
auth envelope (`access_token` / `refresh_token` / `expires_in`) minted by
`jwt::Jwt`. What differs per flow is only how the caller proves who they are:

```
proof ──verify──▶ email ──add_or_get_user──▶ user_id ──generate_auth_response──▶ tokens
(code / ID token / OAuth code)         (health_app_user row)
```

The module is two units — the HTTP front end (`service.*`) and email-code
delivery (`email.*`). (Tanka QR-code sign-in was web-only and left with the web
client; it is preserved at the `v2-full-2026-08` tag.) Everything else a flow
needs (token verification, OAuth code exchange) is borrowed from `src/jwt` and
`src/client`, so adding a provider is a self-contained route.

## `service.{hpp,cpp}` — `UserService`

The composition root and route owner. Constructing it borrows the `Config`,
`Database`, `Cache`, `Jwt`, and the optional Firebase/Apple token validators,
builds the email validator and the per-provider web-config blobs from config,
and wires every route onto the `Router` — so a caller just instantiates it once
at startup. Three private helpers are the shared spine all routes converge on:

- **`add_or_get_user(email)`** — look up the active user by (lowercased) email,
  inserting the row if absent. The email/Google/Apple/GitHub flows all land here.
- **`add_or_get_wechat_user(openid, unionid)`** — the WeChat sibling, keyed on the
  stable `wechat_openid` column; a created row gets a synthetic `<id>@wechat`
  address so the NOT NULL + unique email column is satisfied for accounts with no
  real email.
- **`generate_auth_response(res, user_id, email)`** — mint the access + refresh
  token pair (refresh lives 2× as long) and write the standard envelope.

Routes registered (`register_routes`):

- **`POST /email/login`** → send a verification code to the address (delegates to
  the email validator, which handles validation, predefined-address
  short-circuits, and the resend cooldown).
- **`POST /email/verify`** → check the code, `add_or_get_user`, return tokens.
- **`GET|POST /firebase/verify`** → a **generic Firebase ID-token verifier** —
  every Firebase-brokered provider lands here, none of them get a dedicated route.
  Which OAuth provider authenticated the user (Google, X.com/Twitter,
  GitHub-via-Firebase, …) is chosen on the client and in the Firebase console; by
  the time the token arrives it's just a Firebase RS256 ID token and this route
  treats every provider identically. GET serves the
  Firebase web-app config (so the client can init the Firebase JS SDK / hide the
  button when unconfigured); POST verifies the token via
  `jwt::FirebaseTokenValidator` (signature + `iss` + `aud` + `exp`), then
  `add_or_get_user` by the token's email. **Caveat:** the validator requires a
  non-empty `email` claim, so a provider that doesn't return one (notably X.com /
  Twitter, which often omits email) fails here — this build keys Firebase users
  purely on email and has no openid-style fallback like `/wechat/verify`.
- **`GET|POST /apple/verify`** → "Sign in with Apple": GET serves `{clientId}`;
  POST verifies the Apple ID token via `jwt::AppleTokenValidator` (RS256 + iss +
  aud + exp + email).
- **`GET|POST /wechat/verify`** → GET serves `{appid}`; POST exchanges a login
  `code` for openid/unionid via WeChat. Three code sources are selected by the
  optional `flow` field — Mini Program (`jscode2session`, default), web, and
  mobile-app (both `sns/oauth2/access_token`) — each with its own credentials,
  falling back to the Mini Program pair when unset.
- **`GET|POST /github/verify`** → GET serves `{clientId}`; POST does the
  server-side OAuth exchange (`code` → access token → primary verified email from
  `/user/emails`, falling back to the public profile email), keeping the secret
  server-side throughout.

Each provider is configured independently: the credentials are copied from
`Config` in the ctor, and a provider whose credentials are unset reports
"not configured" (GET → non-zero code so the client hides the button; POST →
error), so the build degrades cleanly to whatever's wired up. Failures within a
POST use sequential negative codes; the `msg` carries the human-readable reason.

## `email.{hpp,cpp}` — verification codes

`EmailCodeValidator` sends and verifies short numeric codes, returning
`std::nullopt` on success or a human-readable error otherwise (the Python
`str | None` contract, where `None` means "ok"). `create_email_validator(opts,
cache)` picks the concrete impl by what's configured, in priority order:

1. **Mandrill** — when `smtp_host` points at `mandrillapp.com` (the SMTP password
   doubles as the API key), or
2. **SMTP** — direct libcurl delivery (port 465 ⇒ implicit TLS, else STARTTLS), or
3. **Mandrill** — via an explicit API key + template, or
4. **Dummy** — no transport; real sends report the missing transport, but
   predefined addresses still verify so demo logins flow without SMTP.

The first three share `StoringEmailValidator`, which holds the common flow —
predefined-code short-circuit (by exact address or by domain), send cooldown,
code persistence with TTL, and single-use verification — around a pluggable
`deliver()`. Issued codes and cooldowns live in `cache::Cache`, so the validator is single-threaded by design (driven from the
server's service thread). Codes are cryptographically random and drawn without
modulo bias.

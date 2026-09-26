#pragma once

#include "cache/cache.hpp"
#include "config/store.hpp"
#include "database/database.hpp"
#include "storage/storage.hpp"

#include <cstdint>
#include "compat/cxx11.hpp"
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace mirobody {

struct UpstreamConfig {
    std::string base_url;
    std::string realtime_url;
    std::string api_key;
};

//------------------------------------------------------------------------------

// Azure OpenAI resource identity. Distinct from UpstreamConfig because Azure
// composes its URL from {endpoint}/openai/deployments/{deployment} and pins the
// request schema with an api-version query param, and authenticates with an
// `api-key:` header rather than `Authorization: Bearer`. Loaded from the
// AZURE_OPENAI_* keys; see load_azure_config. Empty endpoint => Azure unused.
struct AzureConfig {
    std::string key;                          // AZURE_OPENAI_KEY (alias: AZURE_OPENAI_API_KEY)
    std::string endpoint;                     // https://{resource}.openai.azure.com
    std::string deployment;                   // e.g. gpt-4o-mini-prod
    std::string api_version = "2024-10-21";   // AZURE_OPENAI_API_VERSION; current GA
};

//------------------------------------------------------------------------------

// Long-term memory subsystem (see src/memory/). The memory module stores
// durable facts per user and recalls the most relevant ones into an agent turn,
// surfaced to agents as the `remember` / `recall_memory` MCP tools. `provider`
// selects the backend, mirroring how EMBEDDING_PROVIDER selects an embedder:
//   - "local" (default) -> LocalMemory: facts plus their 1024-dim embeddings in
//                          the app database, ranked by in-process cosine over the
//                          caller's own rows. No extra services.
//   - "none"            -> disabled (the tools report memory is unavailable).
// Hosted memory services (Mem0, Zep, EverOS) are a server concern and live in
// the main mirobody repo; on a phone the facts stay in the local database.
// Set via MEMORY_PROVIDER / MEMORY_TOP_K.
struct MemoryConfig {
    std::string provider = "local";
    int         top_k    = 5;      // default recall count when the caller omits one
};

//------------------------------------------------------------------------------

//------------------------------------------------------------------------------

struct JwtConfig {
    std::string key;
    std::string iss;
    std::string aud;
    std::string client_id;
    std::string scope;
    std::string salt;

    // RS256 (asymmetric) signing material. When private_key_pem is set the
    // server signs tokens with RS256 instead of HS256 and publishes the matching
    // public key as a JWKS (/.well-known/jwks.json) so third-party resource
    // servers can verify mirobody-issued tokens without the shared secret.
    // public_key_pem is optional — when omitted it is derived from the private
    // key. From JWT_PRIVATE_KEY / JWT_PUBLIC_KEY (PEM). Empty => HS256 (JWT_KEY).
    // When private_key_pem is set it TAKES PRECEDENCE: the server signs/verifies
    // with RS256 and `key` (JWT_KEY) is ignored.
    std::string private_key_pem;
    std::string public_key_pem;

    // Additional verify-only RSA public keys for key rotation, from the
    // suffixed JWT_PUBLIC_KEY_2 / JWT_PUBLIC_KEY_3 / ... YAML keys (scanned until
    // a gap). They are published in the JWKS alongside the active key and accepted
    // when verifying, so tokens signed by a previous key keep validating while it
    // is being rotated out. The active signer stays the single JWT_PRIVATE_KEY.
    std::vector<std::string> additional_public_key_pems;
};

//------------------------------------------------------------------------------

// OAuth 2.0 *authorization server* settings — the surface that lets MCP clients
// (and any other third-party OAuth client) obtain access tokens for this server
// via the authorization-code + PKCE flow. The tokens it issues are the same
// mb_oauth HS256 JWTs the rest of the system mints (see JwtConfig); this only
// configures the protocol endpoints around them. All fields have working
// defaults, so OAuth works out of the box once JWT_KEY is set.
struct OAuthConfig {
    // Public base URL advertised as the `issuer` in discovery metadata and used
    // to build absolute endpoint URLs (OAUTH_ISSUER, e.g.
    // "https://api.example.com"). Empty (the default) => derived per-request
    // from the Host header and scheme, which is correct for single-origin
    // deployments and dev. Set it when the server sits behind a proxy whose
    // external origin differs from the Host it sees.
    std::string issuer;

    // SPA path the /oauth/authorize endpoint redirects the browser to so the
    // user can sign in (reusing the existing web login UI) and approve the
    // request. The pending-request handle is appended as `?oauth_consent=<id>`.
    // OAUTH_CONSENT_PATH; defaults to the app root. The uri_prefix is prepended
    // automatically, so give a root-relative path here.
    std::string consent_path = "/";

    // Lifetime of a single-use authorization code, seconds (OAUTH_CODE_TTL).
    // Short by design — the client exchanges it for tokens immediately.
    std::int64_t code_ttl = 600;             // 10 minutes

    // Lifetime of a pending authorize request (between /oauth/authorize and the
    // user's consent decision), seconds (OAUTH_AUTHZ_TTL).
    std::int64_t authz_ttl = 600;            // 10 minutes

    // Lifetime of a dynamically-registered client record, seconds
    // (OAUTH_CLIENT_TTL). Long-lived; mirrors the personal-MCP secret TTL.
    std::int64_t client_ttl = 60 * 60 * 24 * 365;   // 1 year

    // Space-delimited scopes advertised in discovery and granted by default
    // when a client requests none (OAUTH_SCOPES). Defaults to the MCP scopes.
    std::string scopes_supported = "mcp:read mcp:write";

    // Path of the protected resource this server guards — the MCP endpoint. Used
    // to build the RFC 9728 protected-resource metadata `resource` value and the
    // WWW-Authenticate challenge. OAUTH_RESOURCE_PATH; uri_prefix is prepended.
    std::string resource_path = "/mcp";

    // Optional space/comma-delimited host allowlist for https redirect URIs
    // (OAUTH_ALLOWED_REDIRECT_HOSTS). Empty (the default) accepts any https
    // host, plus loopback http and custom-scheme URIs, per RFC 8252 for native
    // apps. When non-empty, an https redirect URI is rejected unless its host is
    // listed (loopback and custom schemes remain allowed).
    std::string allowed_redirect_hosts;
};

//------------------------------------------------------------------------------

// SMART-on-FHIR *client* settings — for connecting a user's EHR (Epic, Oracle
// Health/Cerner, …) and pulling their records, driven by health::EhrConnectService.
// Distinct from OAuthConfig above (where mirobody is the authorization *server*):
// here mirobody is an OAuth *client* of the EHR. The app is registered once with
// each EHR vendor to obtain `client_id`; `redirect_uri` is this server's callback
// (/health/ehr/callback) and must match the registration. `client_secret` is set
// only for a confidential client; public clients rely on PKCE alone. Empty
// client_id/redirect_uri => the connect flow reports "not configured".
struct SmartFhirConfig {
    std::string client_id;        // SMART_FHIR_CLIENT_ID
    std::string client_secret;    // SMART_FHIR_CLIENT_SECRET (optional)
    std::string redirect_uri;     // SMART_FHIR_REDIRECT_URI (absolute; matches /health/ehr/callback)
    // Default scope: patient-context launch + read of the resources we ingest.
    std::string scope = "launch/patient openid fhirUser offline_access patient/Observation.read";
    std::int64_t state_ttl = 600; // pending-authorize TTL, seconds (SMART_FHIR_STATE_TTL)
};

//------------------------------------------------------------------------------

// Verification-code email delivery, from the EMAIL_* YAML keys. Consumed by
// user::create_email_validator, which auto-selects a transport: direct SMTP
// (smtp_host + smtp_user + smtp_pass + from_email) or the Mandrill HTTP API
// (smtp_host pointing at *.mandrillapp.com with the API key in smtp_pass, plus
// template). All empty => no real delivery (predefined codes only).
struct EmailConfig {
    std::string smtp_host;
    int         smtp_port = 0;     // 0 -> defaulted to 465 (SMTPS) downstream
    std::string smtp_user;
    std::string smtp_pass;
    std::string from_email;
    std::string from_name;
    std::string template_name;     // Mandrill template; ignored for direct SMTP
};

//------------------------------------------------------------------------------

// Chat limits. Each agent / live turn on /api/chat drives an upstream LLM call
// (tokens + latency + cost), so a per-user rolling-window cap guards against
// runaway loops and abuse. Defaults to 0 = unlimited (no behavior change); set a
// positive value to enforce it. Applies to both the SSE and WebSocket paths
// (they share the dispatcher). See src/chat/dispatcher.cpp.
struct ChatConfig {
    int rate_max_per_window = 0;     // agent/live turns per user per window (0 = unlimited)
    int rate_window_seconds = 60;    // length of that window, in seconds

    // How long the /api/chat SSE stream may go without sending anything before a
    // keepalive comment is emitted (0 = never). A turn has genuinely silent
    // stretches -- text extraction from an upload runs a vision model
    // (FILE_PARSER_TIMEOUT_MS allows it five minutes), and a thinking model
    // deliberates before its first token -- and a connection that carries no
    // bytes for that long is cut by whatever sits in the middle: nginx's
    // proxy_read_timeout is 60s by default, tunnels and load balancers are
    // similar. This server does not time the stream out itself (see the
    // NO_PENDING_TIMEOUT note in server/router.cpp), so the keepalive exists for
    // the hops in between. Keep it well under the strictest one.
    int sse_heartbeat_seconds = 15;
};

//------------------------------------------------------------------------------

// Care-circle limits. All caps default to 0, meaning "unlimited" (the prior
// behavior); set a positive value to bound growth and abuse. See src/circle.
struct CircleConfig {
    int max_circles_per_user   = 0;     // owned active circles per user (0 = unlimited)
    int max_members_per_circle = 0;     // active members (incl. pending) per circle (0 = unlimited)
    int invite_max_per_window  = 20;    // per-inviter invitations allowed per window (0 = unlimited)
    int invite_window_seconds  = 3600;  // length of the invite rate-limit window
};

//------------------------------------------------------------------------------

struct Config {
    std::string listen_addr = "0.0.0.0";
    std::uint16_t listen_port = 8080;

    // URI prefix prepended to every registered route (HTTP + WebSocket) and
    // stripped from request paths before static files are resolved, from the
    // HTTP_URI_PREFIX YAML key. Lets the whole app live under a sub-path (e.g.
    // "/mirobody") behind an ingress that does NOT strip the prefix. Normalized
    // on load to a leading-slash, no-trailing-slash form. Empty (the default)
    // serves everything at the root, exactly as if unset.
    std::string uri_prefix;

    // Largest request body this server accepts, in bytes, from HTTP_MAX_BODY_BYTES
    // (0 = unbounded). The bound the upload path relies on: a body is buffered
    // whole in memory before dispatch, and a chat upload's bytes are then copied
    // down the attachment chain and base64-expanded into the model request, so
    // the peak cost of one request is several times this value. Enforced by the
    // router for every route -- at headers time from Content-Length, and again as
    // the body arrives (a chunked upload declares no length, and a declared one
    // can lie); over it, the request is answered 413 and the connection closed.
    // The default leaves room for a photo or a scanned PDF while keeping a single
    // request far below the providers' own inline caps (Gemini rejects a request
    // whose inline payload exceeds 20 MB).
    std::int64_t max_request_body_bytes = 32 * 1024 * 1024;   // 32 MiB

    // Public origin of this server / its web app, e.g. "https://app.example.com"
    // (no trailing slash), from PUBLIC_BASE_URL. Used to build absolute links in
    // outbound email -- currently the care-circle invite link
    // (<public_base_url>/circle/accept?token=...). Empty falls back to the OAuth
    // issuer; when neither is set the invite email omits the link and the invitee
    // accepts in-app instead.
    std::string public_base_url;

    UpstreamConfig openai{
        "https://api.openai.com",
        "wss://api.openai.com/v1/realtime",
        ""
    };
    UpstreamConfig gemini{
        "https://generativelanguage.googleapis.com",
        "wss://generativelanguage.googleapis.com/ws/google.ai.generativelanguage.v1beta.GenerativeService.BidiGenerateContent",
        ""
    };
    // DashScope (Alibaba Cloud), an OpenAI-compatible upstream used for Qwen LLM
    // and qwen embeddings. base_url points at the compatible-mode root, so
    // consumers append the OpenAI-style path tail (e.g. "/embeddings"). Set via
    // DASHSCOPE_BASE_URL / DASHSCOPE_REALTIME_URL / DASHSCOPE_API_KEY.
    UpstreamConfig dashscope{
        "https://dashscope.aliyuncs.com/compatible-mode/v1",
        "",
        ""
    };
    // DeepSeek, an OpenAI-compatible upstream (deepseek-chat / deepseek-reasoner).
    // base_url is the API root; consumers append the OpenAI-style path tail (e.g.
    // "/chat/completions"). No realtime endpoint. Set via DEEPSEEK_BASE_URL /
    // DEEPSEEK_API_KEY.
    UpstreamConfig deepseek{
        "https://api.deepseek.com",
        "",
        ""
    };
    // NVIDIA NIM (build.nvidia.com), an OpenAI-compatible upstream hosting 100+
    // open models on a free developer tier (~40 RPM). base_url is the API root;
    // consumers append "/v1" then the OpenAI-style path tail. The offered models
    // live in res/agents/baseline.cpp (the NvidiaModel table). Set via
    // NVIDIA_BASE_URL / NVIDIA_API_KEY.
    UpstreamConfig nvidia{
        "https://integrate.api.nvidia.com",
        "",
        ""
    };
    // Zhipu GLM (open.bigmodel.cn), an OpenAI-compatible upstream. GLM-4.7-Flash
    // is permanently free with no total cap -- the recommended long-term free
    // domestic option. base_url already includes the "/api/paas/v4" root, so
    // consumers append only the OpenAI-style path tail. Set via ZHIPU_BASE_URL /
    // ZHIPU_API_KEY (GLM_API_KEY also accepted).
    UpstreamConfig zhipu{
        "https://open.bigmodel.cn/api/paas/v4",
        "",
        ""
    };
    // Azure OpenAI resource (optional alternative to the direct openai upstream).
    // Populated from AZURE_OPENAI_* by load_config; endpoint empty => not used.
    AzureConfig azure;

    int connect_timeout_ms = 10000;
    int request_timeout_ms = 60000;

    std::string log_level = "info";

    database::SQLiteConfig     sqlite;

    cache::MemoryKvConfig      memory_kv;

    // Long-term memory backend selection. See MemoryConfig.
    MemoryConfig               memory;



    // Fernet key(s) that encrypt per-user vendor OAuth tokens at rest in
    // user_vendor_accounts (last key encrypts, all decrypt — rotate by appending).
    // From VENDOR_TOKEN_ENCRYPTION_KEY (YAML sequence or comma-separated scalar).
    // When UNSET this falls back to file_encryption_keys (FILE_ENCRYPTION_KEY), since
    // vendor tokens are user data at rest just like uploads — so configuring file
    // encryption enables durable, multi-instance token storage with no extra key. Set
    // VENDOR_TOKEN_ENCRYPTION_KEY only to rotate the token key independently.
    // EMPTY (neither key set) disables DB token storage (tokens are never written in
    // the clear); with no key, /bind/verify still verifies but does not persist tokens
    // (a single-instance deployment then relies on the configured <ID>_* credential).
    std::vector<std::string> vendor_token_encryption_keys;

    // The server callback URL for the web vendor-OAuth connect flow — where a vendor
    // redirects the browser after consent. Must be registered with each vendor and
    // point at this server's GET /vendors/callback, e.g.
    // "https://api.example.com/vendors/callback". Empty disables web Connect (the
    // /vendors/{id}/authorize route then reports "not configured"). VENDOR_REDIRECT_URI.
    std::string vendor_redirect_uri;

    JwtConfig                  jwt;

    // OAuth 2.0 authorization-server settings. See OAuthConfig above.
    OAuthConfig                oauth;

    // SMART-on-FHIR client settings for the EHR connect flow. See SmartFhirConfig.
    SmartFhirConfig            smart_fhir;

    // Chat per-user rate limit (agent/live turns). See ChatConfig.
    ChatConfig                 chat;

    // Care-circle limits (circle count / member count / invite rate). See CircleConfig.
    CircleConfig               circle;

    // The PRIMARY Firebase project: the one published to the web client (see
    // firebase_web_api_key below) and named in the CSP for the sign-in popup.
    // Set via FIREBASE_PROJECT_ID, or taken as the first entry of
    // FIREBASE_PROJECT_IDS.
    std::string firebase_project_id;

    // EVERY Firebase project whose ID tokens this server will accept, primary
    // first. The validator (src/jwt/firebase.*) requires a token's `aud` to equal
    // one of these and its `iss` to be
    // `https://securetoken.google.com/<that id>`.
    //
    // More than one is allowed because clients are built against whatever project
    // they were given, and a token is only valid for the project that issued it:
    // the Android APK ships a google-services.json naming one, while the web
    // client is handed the primary at runtime. Without a set, serving both would
    // mean rebuilding a client per deployment. It costs nothing to verify -- the
    // Google cert bundle is not per-project.
    //
    // Set via FIREBASE_PROJECT_IDS (YAML sequence, or a comma-separated scalar for
    // the env-var form). When unset it is just { firebase_project_id }, so an
    // existing single-project deployment needs no change.
    //
    // Each entry widens who can sign in: anyone able to mint a token in that
    // project. List only projects you control.
    std::vector<std::string> firebase_project_ids;

    // Firebase *web app* config, advertised under "google" by GET /auth/providers
    // so the web client can initialize the Firebase JS SDK for Google sign-in. projectId
    // comes from firebase_project_id; authDomain is not configured here (the
    // client uses its own origin). Set via FIREBASE_WEB_API_KEY /
    // FIREBASE_MESSAGING_SENDER_ID. The web Google button is shown only when the
    // api key and project id are both present.
    std::string firebase_web_api_key;
    std::string firebase_messaging_sender_id;

    // OAuth 2.0 client ID issued by Google Cloud Console. Set via the
    // GOOGLE_CLIENT_ID env var (or the YAML store). The Google ID-token
    // validator under src/jwt/google.* checks that incoming tokens carry
    // this value in their `aud` claim.
    std::string google_client_id;

    // Encryption at rest for user uploads. When FILE_ENCRYPTION_KEY is set
    // (one or more 44-char URL-safe-base64 Fernet keys, e.g. from
    // `Fernet::generate_key()`), every chat upload and its .meta / .trans
    // sidecars are stored Fernet-encrypted, so a leaked S3/OSS credential
    // yields ciphertext rather than file contents (the keys live here, not with
    // the storage secret). With encryption on, file bytes are served only
    // through this server (decrypted in memory) at `file_url_prefix`, since a
    // client can't decrypt bucket bytes itself; `file_url_base` is an optional
    // absolute origin for those URLs (empty => relative same-origin). Empty =>
    // disabled, with uploads stored in the clear exactly as before.
    //
    // Multiple keys support zero-downtime rotation: the LAST key encrypts new
    // writes, and ALL keys are tried on read, so objects written under an older
    // key keep decrypting. Rotate by appending a new key; once nothing is still
    // encrypted under an old key (let it expire, or re-write objects under the
    // newest key), drop it. Accepts a YAML sequence or a comma-separated scalar.
    std::vector<std::string> file_encryption_keys;   // FILE_ENCRYPTION_KEY (last encrypts, all decrypt)

    // Stable secret that seeds object-KEY derivation (the hashed per-user prefix
    // and content digest in a stored key), kept SEPARATE from the rotatable
    // encryption keys so rotating them leaves every object's path unchanged --
    // which is what lets a multi-key read find an object before trying keys on
    // it. REQUIRED when FILE_ENCRYPTION_KEY is set; must NEVER change once data
    // exists (changing it re-paths every object, orphaning them). FILE_KEY_SEED.
    std::string file_key_seed;
    std::string file_url_prefix = "/files";   // FILE_URL_PREFIX
    std::string file_url_base;                 // FILE_URL_BASE (optional absolute origin)

    // Apple "Sign in with Apple" Services ID (the web client_id). Set via the
    // APPLE_CLIENT_ID env var (or the YAML store). The Apple ID-token validator
    // under src/jwt/apple.* checks that incoming tokens carry this value in their
    // `aud` claim; GET /auth/providers advertises it to the web client so it can
    // initialize the Apple JS SDK. Web Apple sign-in is enabled only when set.
    std::string apple_client_id;

    // WeChat Mini Program credentials, used by POST /wechat/verify to call
    // WeChat's jscode2session (the WeChat analog of Firebase token
    // verification): it exchanges the code from the client's wx.login() for the
    // user's openid/unionid. Both must be set for WeChat sign-in to be enabled.
    // Set via WECHAT_APPID / WECHAT_SECRET. wechat_api_base is the jscode2session
    // host (WECHAT_API_BASE), overridable only to point tests at a mock.
    std::string wechat_appid;
    std::string wechat_secret;
    std::string wechat_api_base = "https://api.weixin.qq.com";

    // WeChat *web* credentials, used by the browser sign-in flows that the web
    // client drives: an Open Platform "website application" (QR sign-in via
    // qrconnect, scope snsapi_login) on desktop, or an Official Account web
    // OAuth (oauth2/authorize, scope snsapi_userinfo) inside WeChat. Both go
    // through POST /wechat/verify with {"flow":"web"}, which exchanges the code
    // via sns/oauth2/access_token. These are distinct WeChat platforms from the
    // Mini Program, so they generally carry a different appid/secret -- set via
    // WECHAT_WEB_APPID / WECHAT_WEB_SECRET. When unset they fall back to the Mini
    // Program credentials above (fine for a unified Open Platform appid or for
    // tests pointed at a mock). The web sign-in button is shown only when the
    // effective web appid and secret are both present.
    std::string wechat_web_appid;
    std::string wechat_web_secret;

    // GitHub OAuth (web). Set via GITHUB_CLIENT_ID / GITHUB_CLIENT_SECRET (the
    // OAuth App credentials from github.com -> Settings -> Developer settings ->
    // OAuth Apps). Both must be set to enable GitHub sign-in. The flow mirrors
    // WeChat web: the browser redirects to GitHub's authorize page, bounces back
    // with an OAuth `code`, and POSTs it to /github/verify, which exchanges it
    // server-side (the secret never leaves the server) and reads the user's
    // primary verified email to identify the account. GET /auth/providers
    // advertises the client_id to the web client so it can build the authorize
    // URL; the button is shown only when both credentials are set.
    //
    // github_oauth_base is the authorize/token host (github.com) and
    // github_api_base is the REST API host (api.github.com); both are
    // overridable only to point tests at a mock.
    std::string github_client_id;
    std::string github_client_secret;
    std::string github_oauth_base = "https://github.com";
    std::string github_api_base   = "https://api.github.com";

    // WeChat *mobile app* credentials, used by the native Android / iOS apps:
    // the WeChat OpenSDK returns an OAuth code (scope snsapi_userinfo) that the
    // app POSTs to /wechat/verify with {"flow":"app"}, exchanged via
    // sns/oauth2/access_token. An Open Platform "Mobile Application" is yet
    // another registration distinct from the Mini Program and the website app,
    // so it carries its own appid/secret -- set via WECHAT_OPEN_APPID /
    // WECHAT_OPEN_SECRET. When unset they fall back to the web, then the Mini
    // Program credentials.
    std::string wechat_app_appid;
    std::string wechat_app_secret;

    // Predefined email -> verification code pairs for testing/demo login, from
    // the EMAIL_PREDEFINE_CODES YAML map. Each entry lets an email log in with
    // the mapped code instead of a real emailed one. Empty (the default) means
    // no demo logins. Printed at startup so the operator knows the test creds.
    std::unordered_map<std::string, std::string> email_predefine_codes;

    // Predefined domain -> verification code pairs, from the
    // EMAIL_PREDEFINE_DOMAIN_CODES YAML map. Any address whose domain matches a
    // key (and isn't listed individually in email_predefine_codes) logs in with
    // the mapped code. Lets one entry stand in for a whole demo domain (e.g.
    // "demo" -> "000000" covers user0@demo .. userN@demo) instead of enumerating
    // every address. Empty (the default) means no domain-wide demo logins.
    std::unordered_map<std::string, std::string> email_predefine_domain_codes;

    // Verification-code email transport. See EmailConfig above.
    EmailConfig email;

    // Response headers added to every HTTP response, from the HTTP_HEADERS YAML
    // map. Typically the CORS Access-Control-* set; when present, the router
    // also answers OPTIONS preflight with them. Empty => no extra headers.
    std::unordered_map<std::string, std::string> http_headers;

    // Filesystem directories served as static content for GET requests that
    // match no registered API route, from the HTTP_ROOT YAML key. A request for
    // a mount root (or any path ending in "/") serves "index.html" from that
    // directory; relative paths are resolved against the process working
    // directory.
    //
    // HTTP_ROOT is either a bare directory string -- one frontend at "/" (the
    // original single-frontend case) -- or a {mount-path: directory} map, which
    // serves several frontends each at its own URL prefix (mirroring data-mind's
    // HTDOC_DIR). Each entry here is {mount-path, directory}; mount-path is
    // normalized to a leading slash and no trailing slash ("/" for the root),
    // and the list is sorted most-specific-prefix-first so a deeper mount (e.g.
    // "/dashboard") wins over the "/" catch-all. Empty => static serving is
    // disabled (unmatched GETs 404). See HTTP_ROOT.
    std::vector<std::pair<std::string, std::string>> http_roots;

    // Base directory holding the DDL applied at startup, from the SQL_DIR YAML
    // key (default "res/sql"). The files live in a per-backend subdirectory —
    // e.g. "<sql_dir>/pg/*.sql" for PostgreSQL — and are run in filename order.
    // Resolved against the process working directory when relative.
    std::string sql_dir = "res/sql";

    // Whether the server applies the DDL under `sql_dir` at startup (see
    // database::apply_schema). true (the default) runs the idempotent schema
    // init on every boot; set DB_INIT_SCHEMA: false to skip it entirely, for a
    // deployment whose schema is created and migrated out of band and where the
    // serving DB user should not (or cannot) issue DDL. Independent of the ENV
    // gate in Server::start — both must permit the init for it to run.
    bool db_init_schema = true;

    // Underlying key/value store, populated by load_config(). The lazy
    // getters below read from it on demand, so that multi-instance configs
    // distinguished by suffix do not have to be enumerated up front. Held as the local-YAML
    // derivation since that is the highest-priority source; any remote
    // config has already been absorbed into it.
    utils::LocalYamlStore store;

    // Local-filesystem object storage, read on demand from LOCAL_STORAGE_DIR /
    // LOCAL_STORAGE_URL_PREFIX / LOCAL_STORAGE_BASE_URL / LOCAL_STORAGE_SECRET.
    // The no-credentials backend for self-hosted / on-device runs;
    // `local_storage().configured()` is true once LOCAL_STORAGE_DIR is set.
    storage::LocalConfig local_storage() const;

    // Dumps a human-readable summary to stdout with secrets masked (first 3
    // and last 3 chars shown, middle replaced by `******`). Sections whose
    // primary identifier (sqlite.path / jwt.key / ...) is unset are skipped.
    // Intended to be called once at startup to surface config errors before
    // the core runs.
    void print() const;
};

// Settle FIREBASE_PROJECT_ID against FIREBASE_PROJECT_IDS into one primary plus one
// accepted set (see Config::firebase_project_ids). Applied by load_config; exposed
// because load_config also reads a remote config server, so this is the only way to
// exercise the precedence without the network.
void reconcile_firebase_projects(std::string& primary, std::vector<std::string>& all);

Config load_config(const mirobody::optional<std::string>& yaml_path = mirobody::nullopt);

// Process-wide Config, initialized automatically on first access from the same
// sources load_config() reads with no path (MIROBODY_CONFIG env, then
// ./config.yml in the working directory, then built-in defaults). The first
// caller triggers the load under a lock, so concurrent first uses are safe.
//
// Lets embedding entry points (the C API in src/platform/c_api.cpp, the debug
// CLIs) share one config without threading a Config reference through every
// call. Treat it as read-only after startup: the rest of the codebase passes
// `const Config&` around without locking, so reconfiguration after serving has
// begun is not supported (see init_config).
Config& config();

// (Re)load the global config from `yaml_path` (or the default discovery when
// nullopt) and return it. Call once during startup — before any config() reader
// runs — when the host needs the global to come from a specific file rather
// than the default discovery. Returns the freshly loaded instance.
Config& init_config(const mirobody::optional<std::string>& yaml_path = mirobody::nullopt);

// Build a populated key-value store from the same sources `load_config` reads:
//
//   1. CONFIG_ENCRYPTION_KEY env -> Fernet decrypter (decrypts `gAAAA…` values)
//   2. CONFIG_SERVER / CONFIG_TOKEN / ENV env -> remote YAML pull
//   3. yaml_path arg OR MIROBODY_CONFIG env OR ./config.yml -> local YAML
//
// Later sources override earlier ones. Once returned, callers read whatever
// keys they need via `store.get_str / get_int / get_bool`, which transparently
// fall back to environment variables (getenv > stored YAML > caller default).
//
// Intended as the entry point for the debug CLIs under cli/ so they share
// the server's config discovery without dragging in server-specific schema.
utils::LocalYamlStore load_config_store(const mirobody::optional<std::string>& yaml_path = mirobody::nullopt);

// Read the Azure OpenAI resource fields from an already-loaded store. Shared by
// load_config (to populate Config::azure) and the debug CLIs so the AZURE_OPENAI_*
// key names and the key-alias precedence live in one place. The key is taken
// from AZURE_OPENAI_KEY, falling back to AZURE_OPENAI_API_KEY when that is empty
// (so both spellings the CLIs historically used are accepted). Unset keys keep
// the AzureConfig defaults (notably api_version).
AzureConfig load_azure_config(const utils::LocalYamlStore& store);

}

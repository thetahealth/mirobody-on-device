#include "server/server.hpp"

#include "chat/agent.hpp"
#include "client/http_client.hpp"
#include "config/fernet.hpp"
#include "database/schema.hpp"
#include "platform/log.hpp"
#include "storage/sign.hpp"

#include <libwebsockets.h>

#include <array>
#include <exception>
#include <memory>

namespace mirobody {

//------------------------------------------------------------------------------
// Construction / teardown
//------------------------------------------------------------------------------

Server::Server(Config cfg) : cfg_(std::move(cfg)) {}

Server::~Server() {
    stop_and_join();
}

//------------------------------------------------------------------------------
// Lifecycle
//------------------------------------------------------------------------------

bool Server::start() {
    if (running_.load()) return true;

    platform::log_info("starting mirobody server...");

    // Backing resources for the request handlers. Any failure here aborts the
    // start: opening a connection or building the signer can throw, and the
    // JWT signing key is mandatory because the user service issues tokens.
    try {
        // Open the SQL backend selected at build time (MIROBODY_DATABASE_BACKEND,
        // see CMakeLists.txt). Each branch reads its own config struct and routes
        // through that backend's open(); the rest of the server uses the generic
        // database::Database, so only this block is backend-specific.
        platform::log_info("[1/7] database backend (compile-time): %s",
                           MIROBODY_DATABASE_BACKEND_DIR);
        const database::SQLiteConfig sqlite = cfg_.sqlite;
        platform::log_info("[1/7] opening SQLite database at %s...",
                           sqlite.path.empty() ? ":memory:" : sqlite.path.c_str());
        db_ = std::unique_ptr<database::Database>(new database::Database(sqlite.open()));
        platform::log_info("[1/7] SQLite database ready");

        // Apply the DDL before the server opens its serving connections. The files
        // live under <sql_dir>/MIROBODY_DATABASE_BACKEND_DIR, a per-backend subdir
        // fixed at build time (e.g. "pg" / "pg_legacy" for PostgreSQL — see the
        // database backend selection in CMakeLists.txt). A throwaway connection is
        // used so migration is decoupled from the pool the server runs on.
        // Idempotent (IF NOT EXISTS), so it is safe on every boot.
        //
        // Skipped in the shared/managed environments (TEST, GRAY, PROD), where the
        // schema is owned and migrated out-of-band rather than by each booting
        // server. ENV is the same deployment-environment selector used for the
        // remote config pull (see config.cpp / README "Remote config"); the match is
        // case-insensitive so "prod" and "PROD" both count.
        //
        // Also skipped when DB_INIT_SCHEMA is false — an explicit opt-out for a
        // deployment that manages its schema out of band and whose serving DB user
        // must not issue DDL. Defaults true, so the init runs unless disabled.
        if (const char* env = cfg_.db_init_schema ? std::getenv("ENV") : nullptr) {
            std::string env_upper(env);
            for (char& c : env_upper) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));

            try {
                mirobody::database::apply_schema(*db_, cfg_.sql_dir + "/" MIROBODY_DATABASE_BACKEND_DIR);
            } catch (const std::exception& e) {
                mirobody::platform::log_error("database init failed: %s", e.what());
                mirobody::client::HttpClient::global_cleanup();
                return false;
            }
        }

        cache_ = std::unique_ptr<cache::Cache>(new cache::Cache(cfg_.memory_kv.open()));
        platform::log_info("[2/7] cache backend ready (in-memory)");

        // Object storage for uploads: the local filesystem. Left null when it is
        // not configured; handlers that need uploads must check for it.
        if (cfg_.local_storage().configured()) {
            platform::log_info("[3/7] initializing object storage (local filesystem)...");
            storage_ = cfg_.local_storage().open();
            platform::log_info("[3/7] object storage ready (local filesystem)");
        } else {
            storage_ = nullptr;
            platform::log_info("[3/7] object storage disabled (LOCAL_STORAGE_DIR not set)");
        }

        // Signer: RS256 (asymmetric) when a private key is configured, so the
        // OAuth authorization server can publish a JWKS for third-party token
        // verification; otherwise HS256 (symmetric) from JWT_KEY. Exactly one
        // key source is required.
        if (!cfg_.jwt.private_key_pem.empty()) {
            platform::log_info("[4/7] building JWT signer (RS256)...");
            // RS256 takes precedence; a stray JWT_KEY would otherwise look active.
            if (!cfg_.jwt.key.empty()) {
                platform::log_warn("JWT_KEY is set but ignored: JWT_PRIVATE_KEY selects RS256 "
                                   "(JWT_KEY only applies in HS256 mode)");
            }
            jwt::JwtRs256::Options jwt_opts;
            jwt_opts.private_key_pem = cfg_.jwt.private_key_pem;
            // The server must verify its own tokens (e.g. on /mcp), which needs
            // the public key. Use the configured one, else derive it from the
            // private key so configuring JWT_PRIVATE_KEY alone is sufficient.
            jwt_opts.public_key_pem = cfg_.jwt.public_key_pem.empty()
                ? jwt::rsa_public_pem_from_private(cfg_.jwt.private_key_pem)
                : cfg_.jwt.public_key_pem;
            // Extra verify-only public keys (rotation): published in the JWKS and
            // accepted on verify, never used to sign.
            jwt_opts.additional_public_key_pems = cfg_.jwt.additional_public_key_pems;
            jwt_opts.salt = cfg_.jwt.salt;
            if (!cfg_.jwt.iss.empty())       jwt_opts.iss       = cfg_.jwt.iss;
            if (!cfg_.jwt.aud.empty())       jwt_opts.aud       = cfg_.jwt.aud;
            if (!cfg_.jwt.client_id.empty()) jwt_opts.client_id = cfg_.jwt.client_id;
            if (!cfg_.jwt.scope.empty())     jwt_opts.scope     = cfg_.jwt.scope;
            std::unique_ptr<jwt::JwtRs256> rs(new jwt::JwtRs256(std::move(jwt_opts)));
            jwks_json_ = rs->jwks_json();   // published at /.well-known/jwks.json
            jwt_ = std::move(rs);
            platform::log_info("[4/7] JWT signer ready (RS256, %s)",
                               jwks_json_.empty() ? "no JWKS" : "JWKS published");
        } else if (!cfg_.jwt.key.empty()) {
            platform::log_info("[4/7] building JWT signer (HS256)...");
            jwt::JwtHs256::Options jwt_opts;
            jwt_opts.key = cfg_.jwt.key;
            jwt_opts.salt = cfg_.jwt.salt;   // keys the subject obfuscation (encode_int64)
            if (!cfg_.jwt.iss.empty())       jwt_opts.iss       = cfg_.jwt.iss;
            if (!cfg_.jwt.aud.empty())       jwt_opts.aud       = cfg_.jwt.aud;
            if (!cfg_.jwt.client_id.empty()) jwt_opts.client_id = cfg_.jwt.client_id;
            if (!cfg_.jwt.scope.empty())     jwt_opts.scope     = cfg_.jwt.scope;
            jwt_ = std::unique_ptr<jwt::JwtHs256>(new jwt::JwtHs256(std::move(jwt_opts)));
            platform::log_info("[4/7] JWT signer ready (HS256)");
        } else {
            platform::log_error("neither JWT_KEY nor JWT_PRIVATE_KEY is configured; cannot start");
            teardown();
            return false;
        }

        // Firebase ID-token validator is optional: built only when a project
        // ID is configured. Routes that need it must handle a null validator.
        // It accepts every project in firebase_project_ids, not just the primary:
        // clients are built against whichever project they were given, and a token
        // is only valid for the one that issued it.
        if (!cfg_.firebase_project_ids.empty()) {
            std::string accepted;
            for (std::size_t i = 0; i < cfg_.firebase_project_ids.size(); ++i) {
                if (i) accepted += ", ";
                accepted += cfg_.firebase_project_ids[i];
            }
            platform::log_info("[5/7] enabling Firebase token validator (accepting: %s)...",
                               accepted.c_str());
            firebase_ = std::unique_ptr<jwt::FirebaseTokenValidator>(
                new jwt::FirebaseTokenValidator(cfg_.firebase_project_ids));
            platform::log_info("[5/7] Firebase token validator ready");
        } else {
            platform::log_info("[5/7] Firebase token validator disabled (no FIREBASE_PROJECT_ID / FIREBASE_PROJECT_IDS)");
        }

        // Apple ID-token validator is optional: built only when a Services ID
        // (client_id) is configured. Routes that need it handle a null validator.
        if (!cfg_.apple_client_id.empty()) {
            platform::log_info("[5/7] enabling Apple token validator (client_id=%s)...",
                               cfg_.apple_client_id.c_str());
            apple_ = std::unique_ptr<jwt::AppleTokenValidator>(
                new jwt::AppleTokenValidator(cfg_.apple_client_id));
            platform::log_info("[5/7] Apple token validator ready");
        } else {
            platform::log_info("[5/7] Apple token validator disabled (no APPLE_CLIENT_ID)");
        }
    } catch (const std::exception& e) {
        platform::log_error("service initialization failed: %s", e.what());
        teardown();
        return false;
    }

    platform::log_info("[6/7] registering routes and domain services...");
    router_ = std::unique_ptr<server::Router>(new server::Router());
    // Must precede every route registration below: the prefix is baked into
    // each route key at registration time (HTTP_URI_PREFIX).
    router_->set_uri_prefix(cfg_.uri_prefix);          // sub-path mount, e.g. /mirobody
    router_->set_default_headers(cfg_.http_headers);   // e.g. CORS from HTTP_HEADERS
    router_->set_http_roots(cfg_.http_roots);          // static files from HTTP_ROOT
    // The bound on one request body (HTTP_MAX_BODY_BYTES): bodies are buffered
    // whole before dispatch, so this is what keeps a single upload from pinning
    // arbitrary memory. 0 disables it.
    router_->set_max_body_bytes(static_cast<std::size_t>(
        cfg_.max_request_body_bytes > 0 ? cfg_.max_request_body_bytes : 0));
    if (cfg_.max_request_body_bytes > 0) {
        platform::log_info("[6/7] request body limit: %lld bytes",
                           (long long)cfg_.max_request_body_bytes);
    } else {
        platform::log_warn("[6/7] request body limit disabled (HTTP_MAX_BODY_BYTES=0): "
                           "one upload can buffer without bound");
    }
    // Serve LocalStorage objects over HTTP at LOCAL_STORAGE_URL_PREFIX. Objects
    // live under LOCAL_STORAGE_DIR (which need NOT be inside HTTP_ROOT) and, when
    // LOCAL_STORAGE_SECRET is set, are gated by the presigned "?expires=&sig="
    // HMAC. The advertised URL (LOCAL_STORAGE_BASE_URL, possibly a CDN) is
    // independent of this serving path; with no url_prefix this server does not
    // serve the objects (e.g. they're hosted externally).
    //
    // LocalStorage is the FALLBACK object store: a cloud backend (S3 / OSS) takes
    // precedence, so when either is configured the local HTTP mount stands down
    // (those objects live in the cloud, not on this server's disk).
    // File encryption at rest (FILE_ENCRYPTION_KEY): store uploads and their
    // .meta/.trans sidecars Fernet-encrypted, and serve their bytes back
    // through THIS server decrypted in memory -- a client can't decrypt bucket
    // bytes, so bucket-direct / disk-served URLs would be useless. When on it
    // supersedes the LocalStorage disk mount below (which would serve
    // ciphertext) for every backend.
    bool file_encryption = false;
    if (storage_ != nullptr && !cfg_.file_encryption_keys.empty()) {
        storage::FileEncryptionResult fe;
        try {
            // Shared with the C API (storage::configure_file_encryption): builds
            // the cipher list, derives the seed / read-URL secrets, and runs the
            // key/seed fingerprint guard. A bad key throws FernetError.
            fe = storage::configure_file_encryption(*storage_, cfg_.file_encryption_keys,
                                                    cfg_.file_key_seed, cfg_.file_url_prefix,
                                                    cfg_.file_url_base);
        } catch (const std::exception& e) {
            platform::log_error("FILE_ENCRYPTION_KEY is invalid (expected 44-char "
                                "URL-safe-base64 Fernet keys): %s", e.what());
            teardown();
            return false;
        }
        if (!fe.ok) {
            // A changed seed, a replaced key list, or a missing seed: refuse to
            // start rather than silently orphan existing uploads.
            platform::log_error("file encryption: %s", fe.error.c_str());
            teardown();
            return false;
        }
        if (fe.enabled) {
            router_->set_file_mount(cfg_.file_url_prefix, storage_.get(), fe.url_secret);
            file_encryption = true;
            platform::log_info("[6/7] file encryption enabled (%zu key%s; newest encrypts), "
                               "served decrypted at %s", cfg_.file_encryption_keys.size(),
                               cfg_.file_encryption_keys.size() == 1 ? "" : "s",
                               cfg_.file_url_prefix.c_str());
        }
    }
    {
        const storage::LocalConfig ls = cfg_.local_storage();
        if (file_encryption) {
            platform::log_info("[6/7] LocalStorage HTTP mount disabled: file encryption is on, "
                               "so objects are served through the decrypting file mount");
        } else if (ls.configured() && !ls.url_prefix.empty()) {
            // Refuse to stand up the mount with a weak signing secret: it would
            // verify against a forgeable key (throws on a blank / too-short one).
            ls.validate_signing_secret();
            router_->set_storage_mount(ls.url_prefix, ls.root, ls.secret);
        }
    }
    register_http_routes();

    // Long-term memory store, shared by the chat agent's tool executor and the
    // MCP endpoint (both reach it through ToolContext). May be null when memory
    // is disabled / a remote provider is misconfigured; the tools tolerate that.
    memory_ = memory::make_memory(cfg_, *db_);

    // Domain services self-register their routes onto the router via the ctor.
    user_service_ = std::unique_ptr<user::UserService>(
        new user::UserService(*router_, cfg_, *db_, *cache_, *jwt_, firebase_.get(), apple_.get()));
    chat_service_ = std::unique_ptr<chat::ChatService>(
        new chat::ChatService(*router_, cfg_, *db_, *cache_, storage_.get(), memory_.get(), *jwt_));
    // Care circles (invite/share).
    circle_service_ = std::unique_ptr<circle::CircleService>(
        new circle::CircleService(*router_, cfg_, *db_, *cache_, *jwt_));
    mcp_service_ = std::unique_ptr<mcp::McpService>(
        new mcp::McpService(*router_, cfg_, *jwt_, *cache_, storage_.get(), memory_.get(), db_.get()));
    // OAuth 2.0 authorization server: issues access tokens for the MCP endpoint
    // via the authorization-code + PKCE flow (discovery, registration, token).
    oauth_service_ = std::unique_ptr<oauth::OAuthService>(
        new oauth::OAuthService(*router_, cfg_, *cache_, *jwt_, jwks_json_));
    fhir_service_ = std::unique_ptr<fhir::FhirService>(
        new fhir::FhirService(*router_, cfg_, *db_, *jwt_));
    vendor_service_ = std::unique_ptr<health::VendorService>(
        new health::VendorService(*router_, cfg_, *db_, *cache_, *jwt_));
    ehr_connect_service_ = std::unique_ptr<health::EhrConnectService>(
        new health::EhrConnectService(*router_, cfg_, *db_, *cache_, *jwt_));

    // Build each registered agent's provider LLM clients from config. Agents
    // self-register at load time (res/agents/*.cpp); this populates their
    // per-provider client store for chat::agent_registry().create(...).
    chat::agent_registry().load_clients(cfg_);
    platform::log_info("[6/7] routes and services registered");

    platform::log_info("[7/7] creating libwebsockets context on %s:%u...",
                       cfg_.listen_addr.empty() ? "0.0.0.0" : cfg_.listen_addr.c_str(),
                       static_cast<unsigned>(cfg_.listen_port));
    lws_context_creation_info info{};
    info.port = cfg_.listen_port;
    info.iface = cfg_.listen_addr.empty() ? nullptr : cfg_.listen_addr.c_str();

    // Security headers. We set these ourselves instead of
    // LWS_SERVER_OPTION_HTTP_HEADERS_SECURITY_BEST_PRACTICES_ENFORCE: lws's
    // built-in CSP is default-src 'none' / script-src 'self' / connect-src 'self',
    // which blocks the web client's Google sign-in (it loads the Firebase SDK from
    // gstatic and calls Google's token endpoints). This CSP allows exactly those
    // origins and keeps the rest locked down.
    //
    // The CSP (and the other document-scoped headers) is emitted ONLY for the HTML
    // document, not on every asset/API response where it has no effect -- see the
    // router->set_document_headers call below. Only nosniff stays global.
    //
    // The Firebase auth domain in frame-src is "<projectId>.firebaseapp.com",
    // derived from cfg_.firebase_project_id -- the PRIMARY project only. Any extra
    // entries in firebase_project_ids exist so native clients' tokens verify; they
    // never open a popup in this browser, so widening the CSP for them would grant
    // frame access nothing needs. Omitted entirely when no project is configured
    // (Google sign-in disabled). Apple's SDK origins
    // (appleid.cdn-apple.com for the JS, appleid.apple.com for the sign-in
    // popup/iframe) are added only when APPLE_CLIENT_ID is set -- the web client
    // uses Apple's own SDK on Apple platforms (iOS/macOS) and falls back to the
    // Firebase apple.com provider elsewhere. csp_header_value_ / sec_headers_ are
    // members so their strings outlive context_ (set_document_headers copies its
    // argument, but the global sec_headers_ array is referenced by lws by pointer).
    const bool apple = !cfg_.apple_client_id.empty();
    csp_header_value_  = "default-src 'self'; ";
    csp_header_value_ += "script-src 'self' https://www.gstatic.com https://apis.google.com";
    if (apple) csp_header_value_ += " https://appleid.cdn-apple.com";
    csp_header_value_ += "; ";
    csp_header_value_ += "connect-src 'self' https://www.gstatic.com https://*.googleapis.com "
                         "https://*.firebaseio.com wss:";
    if (apple) csp_header_value_ += " https://appleid.apple.com";
    csp_header_value_ += "; ";
    csp_header_value_ += "frame-src 'self' https://accounts.google.com https://apis.google.com";
    if (!cfg_.firebase_project_id.empty()) {
        csp_header_value_ += " https://";
        csp_header_value_ += cfg_.firebase_project_id;
        csp_header_value_ += ".firebaseapp.com";
    }
    if (apple) csp_header_value_ += " https://appleid.apple.com";
    csp_header_value_ +=
        "; "
        "img-src 'self' data: blob: https:; "
        "style-src 'self' 'unsafe-inline'; "
        "font-src 'self'; base-uri 'none'; form-action 'self'; frame-ancestors 'none';";

    // Only x-content-type-options is emitted on EVERY response (lws global
    // headers): nosniff hardens MIME handling of every asset, scripts/styles
    // included. The document-scoped headers below (CSP, X-Frame-Options,
    // Referrer-Policy, COOP) have no effect off the HTML document, so they are
    // emitted only when the document itself is served (Router::set_document_headers)
    // rather than bloating every asset and API response with a ~500-byte CSP.
    auto* hdrs = new lws_protocol_vhost_options[1]{};
    hdrs[0] = { nullptr, nullptr, "x-content-type-options:", "nosniff" };
    sec_headers_.reset(hdrs);
    info.headers = hdrs;

    // Document-only security headers, emitted just for the HTML entry. COOP note:
    // signInWithPopup opens a cross-origin popup and polls window.closed; the
    // default COOP severs that handle. same-origin-allow-popups keeps this site
    // isolated while letting it retain control of popups it opens.
    router_->set_document_headers(
        std::string("content-security-policy: ") + csp_header_value_ + "\r\n" +
        "x-frame-options: DENY\r\n"
        "referrer-policy: no-referrer\r\n"
        "cross-origin-opener-policy: same-origin-allow-popups\r\n");

    info.options = LWS_SERVER_OPTION_VALIDATE_UTF8;
    router_->apply_to(info);

    context_ = lws_create_context(&info);
    if (!context_) {
        platform::log_error("lws_create_context failed");
        teardown();
        return false;
    }
    platform::log_info("[7/7] libwebsockets context created");

    stop_flag_.store(false);
    running_.store(true);

    // Surface the demo/test login credentials (EMAIL_PREDEFINE_CODES) so the
    // operator knows which email + verification code can be used to log in.
    if (!cfg_.email_predefine_codes.empty()) {
        platform::log_info("predefined login credentials (%zu):",
                           cfg_.email_predefine_codes.size());
        for (const auto& kv : cfg_.email_predefine_codes) {
            platform::log_info("  %s -> %s", kv.first.c_str(), kv.second.c_str());
        }
    }
    if (!cfg_.email_predefine_domain_codes.empty()) {
        platform::log_info("predefined login domains (%zu):",
                           cfg_.email_predefine_domain_codes.size());
        for (const auto& kv : cfg_.email_predefine_domain_codes) {
            platform::log_info("  *@%s -> %s", kv.first.c_str(), kv.second.c_str());
        }
    }

    platform::log_info("mirobody listening on %s:%u",
                       cfg_.listen_addr.c_str(),
                       static_cast<unsigned>(cfg_.listen_port));
    return true;
}

void Server::run_blocking() {
    if (!context_) return;
    while (!stop_flag_.load()) {
        int rc = lws_service(context_, 0);
        if (rc < 0) break;
    }
    if (context_) {
        lws_context_destroy(context_);
        context_ = nullptr;
    }
    teardown();
    running_.store(false);
}

void Server::teardown() {
    // Reverse of construction order: the router owns handlers capturing the
    // user service, which borrows db_/cache_/jwt_. Release outermost first.
    router_.reset();
    ehr_connect_service_.reset();
    vendor_service_.reset();
    fhir_service_.reset();
    oauth_service_.reset();
    mcp_service_.reset();
    circle_service_.reset();
    chat_service_.reset();
    user_service_.reset();
    firebase_.reset();
    jwt_.reset();
    memory_.reset();   // after the services that borrow it, before db_ it borrows
    storage_.reset();
    cache_.reset();
    db_.reset();
}

void Server::register_http_routes() {
    // /api/health stays open so load balancers / k8s probes can reach it without
    // a token, and stays at its bare path (get_unprefixed) so it is unaffected by
    // HTTP_URI_PREFIX -- probes hit /api/health on the pod without knowing where
    // the app is mounted. Authenticated routes are registered by their respective
    // services (see chat::ChatService, user::UserService).
    router_->get_unprefixed("/api/health", [](const server::Request&, server::Response& res) {
        res.text("ok");
    });
}

void Server::stop() {
    stop_flag_.store(true);
    if (context_) lws_cancel_service(context_);
}

//------------------------------------------------------------------------------
// Background thread control
//------------------------------------------------------------------------------

bool Server::start_in_background() {
    if (!start()) return false;
    thread_ = std::thread([this] { run_blocking(); });
    return true;
}

void Server::stop_and_join() {
    stop();
    if (thread_.joinable()) thread_.join();
}

}

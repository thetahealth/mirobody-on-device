#include "config/config.hpp"
#include <optional>

#include "config/store.hpp"
#include "database/database.hpp"
#include "platform/log.hpp"
#include "config/fernet.hpp"

#include <algorithm>
#include <cctype>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <mutex>
#include <utility>

namespace mirobody {

namespace {

std::string mask(const std::string& s) {
    if (s.empty())    return "<not set>";
    if (s.size() < 6) return "************";
    return s.substr(0, 3) + "******" + s.substr(s.size() - 3);
}

// Normalize a URI prefix to a leading-slash, no-trailing-slash form so it can
// be concatenated directly with a route path (which always starts with '/').
// "" / "/" -> "" (no prefix); "mirobody" / "/mirobody/" -> "/mirobody".
std::string normalize_uri_prefix(std::string p) {
    while (!p.empty() && p.back() == '/') p.pop_back();
    if (p.empty()) return p;
    if (p.front() != '/') p.insert(p.begin(), '/');
    return p;
}

// Trim ASCII whitespace from both ends of a string.
std::string trim_ws(const std::string& s) {
    std::size_t b = 0, e = s.size();
    while (b < e && std::isspace(static_cast<unsigned char>(s[b])))     ++b;
    while (e > b && std::isspace(static_cast<unsigned char>(s[e - 1]))) --e;
    return s.substr(b, e - b);
}

// Normalize a static-file mount path to a leading-slash, no-trailing-slash
// form, so "label", "/label", and "/label/" all map to "/label" and ""/"/"
// map to the root "/".
std::string normalize_mount_path(std::string p) {
    p = trim_ws(p);
    while (!p.empty() && p.front() == '/') p.erase(p.begin());
    while (!p.empty() && p.back()  == '/') p.pop_back();
    return "/" + p;
}

// HTTP_ROOT is either a bare directory string -- one frontend at "/" (the
// original single-frontend case) -- or a {mount-path: directory} map serving
// several frontends each at its own URL prefix (mirroring data-mind's
// HTDOC_DIR; via an env var, pass the map as a JSON object string). Resolve
// either form into an ordered {mount, dir} list: mount paths normalized,
// blank directories dropped, and sorted most-specific-prefix-first so the
// router tries a deeper mount ("/dashboard") before the "/" catch-all.
std::vector<std::pair<std::string, std::string>>
read_http_roots(const utils::ConfigStore& store) {
    std::unordered_map<std::string, std::string> map = store.get_dict("HTTP_ROOT");
    if (map.empty()) {
        std::string one = store.get_str("HTTP_ROOT");
        if (!one.empty()) map.emplace("/", one);
    }

    std::vector<std::pair<std::string, std::string>> roots;
    for (const auto& kv : map) {
        std::string dir = trim_ws(kv.second);
        if (dir.empty()) continue;
        roots.emplace_back(normalize_mount_path(kv.first), dir);
    }
    std::sort(roots.begin(), roots.end(),
              [](const std::pair<std::string, std::string>& a,
                 const std::pair<std::string, std::string>& b) {
                  return a.first.size() > b.first.size();
              });
    return roots;
}

// Path of a sibling config file that inserts ".<infix>" before the extension:
//   config.yml         + example -> config.example.yml
//   /etc/mb/config.yml + example -> /etc/mb/config.example.yml
//   dev.yaml            + example -> dev.example.yaml
// A path with no extension just gets ".<infix>" appended.
std::string sibling_config_path(const std::string& base, const std::string& infix) {
    std::string::size_type slash = base.find_last_of("/\\");
    std::string::size_type dot   = base.find_last_of('.');
    bool has_ext = dot != std::string::npos &&
                   (slash == std::string::npos || dot > slash);
    if (!has_ext) return base + "." + infix;
    return base.substr(0, dot) + "." + infix + base.substr(dot);
}

}

//------------------------------------------------------------------------------

utils::LocalYamlStore load_config_store(const std::optional<std::string>& yaml_path) {
    // Enable Fernet decryption for "gAAAA..." values when
    // CONFIG_ENCRYPTION_KEY is present in the environment. The key is run
    // through ConfigStore::derive_fernet_key to match the Python loader.
    std::shared_ptr<encrypt::Fernet> encrypter;
    const char* k = std::getenv("CONFIG_ENCRYPTION_KEY");
    if (k && *k) {
        encrypter = std::make_shared<encrypt::Fernet>(
            utils::ConfigStore::derive_fernet_key(k));
    }

    // Resolve the operator's config path: explicit argument > MIROBODY_CONFIG
    // env var > ./config.yml in the current working directory. This is the file
    // the operator creates and edits; unlike before it need not exist — when
    // absent, only the committed template (and any remote config) apply.
    std::optional<std::string> path = yaml_path;
    if (!path) {
        const char* p = std::getenv("MIROBODY_CONFIG");
        if (p && *p) path = std::string(p);
    }
    if (!path) path = std::string("config.yml");

    // Three config layers, applied lowest precedence first; each absorb
    // overwrites the previous on a key clash, and environment variables override
    // all of them at lookup time (ConfigStore::get_str). Final precedence,
    // high -> low:
    //   1. environment variables
    //   2. local config.yml          (operator-created override)
    //   3. remote config server
    //   4. local config.example.yml  (committed template / defaults)
    // ENV selects the remote config only; it no longer affects which local
    // files are loaded.
    utils::LocalYamlStore store(std::string{}, encrypter);

    // Tier 4 (base): the committed template config.example.yml, found beside
    // the resolved config path. Existence is probed before opening (via fopen,
    // because the supported phone toolchains use the portable directory API here) so a missing template is
    // silent rather than a load() warning.
    std::string example_path = sibling_config_path(*path, "example");
    if (FILE* fh = std::fopen(example_path.c_str(), "rb")) {
        std::fclose(fh);
        utils::LocalYamlStore example_store(example_path, encrypter);
        if (example_store.load()) {
            store.absorb(example_store);
            platform::log_info("config: loaded %s", example_path.c_str());
        }
    }

    // Environment name, lower-cased so "ENV=Prod" and "ENV=prod" resolve to the
    // same remote URL.
    std::string env;
    if (const char* e = std::getenv("ENV")) {
        env = e;
        for (std::size_t i = 0; i < env.size(); ++i) {
            env[i] = static_cast<char>(std::tolower(static_cast<unsigned char>(env[i])));
        }
    }

    // Tier 3: remote config server, layered OVER the template so remote wins.
    const char* srv = std::getenv("CONFIG_SERVER");
    const char* tok = std::getenv("CONFIG_TOKEN");
    if (srv && *srv && tok && *tok && !env.empty()) {
        utils::RemoteYamlStore remote(srv, tok, env, 10000, encrypter);
        if (remote.load()) {
            store.absorb(remote);
            platform::log_info("config: remote loaded from %s (env=%s)", srv, env.c_str());
        } else {
            platform::log_warn("config: remote load failed (%s, env=%s): %s",
                               srv, env.c_str(), remote.last_error().c_str());
        }

        std::string env_mirobody = env;
        env_mirobody.append("_mirobody");
        utils::RemoteYamlStore remote_mirobody(srv, tok, env_mirobody, 10000, encrypter);
        if (remote_mirobody.load()) {
            store.absorb(remote_mirobody);
            platform::log_info("config: remote loaded from %s (env=%s)", srv, env_mirobody.c_str());
        } else {
            platform::log_warn("config: remote load failed (%s, env=%s): %s",
                               srv, env.c_str(), remote_mirobody.last_error().c_str());
        }
    }

    // Tier 2: the operator's own config.yml, layered OVER remote so it wins.
    // Existence is probed before opening so the common "no local file yet" case
    // is silent rather than a load() warning.
    if (FILE* fh = std::fopen(path->c_str(), "rb")) {
        std::fclose(fh);
        utils::LocalYamlStore user_store(*path, encrypter);
        if (user_store.load()) {
            store.absorb(user_store);
            platform::log_info("config: loaded %s", path->c_str());
        }
    }

    // Tier 1 (environment variables) is applied per-lookup by get_str, not here.
    return store;
}

//------------------------------------------------------------------------------

AzureConfig load_azure_config(const utils::LocalYamlStore& store) {
    AzureConfig az;
    az.endpoint    = store.get_str("AZURE_OPENAI_ENDPOINT",    az.endpoint);
    az.deployment  = store.get_str("AZURE_OPENAI_DEPLOYMENT",  az.deployment);
    az.api_version = store.get_str("AZURE_OPENAI_API_VERSION", az.api_version);
    // Key alias: AZURE_OPENAI_KEY (openai_chat's historical name) wins; fall
    // back to AZURE_OPENAI_API_KEY (openai_realtime's name) so either is honored.
    az.key = store.get_str("AZURE_OPENAI_KEY");
    if (az.key.empty()) az.key = store.get_str("AZURE_OPENAI_API_KEY");
    return az;
}

//------------------------------------------------------------------------------

// Settle FIREBASE_PROJECT_ID against FIREBASE_PROJECT_IDS into one primary + one
// accepted set. Split out of load_config so it can be tested without the network:
// load_config also pulls a remote config store, which would supply these very keys.
//
// Rules, in the order they matter:
//   - no list        -> the set is just the primary (the single-project deployment).
//   - no primary     -> the list's first entry becomes it; something has to be the
//                       project published to the web client and named in the CSP.
//   - primary absent
//     from the list  -> PREPENDED, never dropped. A deployment that adds the list
//                       for a second client and forgets to repeat the primary would
//                       otherwise stop accepting its own web client's tokens, with
//                       nothing in the logs to say why.
void reconcile_firebase_projects(std::string& primary, std::vector<std::string>& all) {
    if (all.empty()) {
        if (!primary.empty()) all.push_back(primary);
        return;
    }
    if (primary.empty()) {
        primary = all.front();
        return;
    }
    if (std::find(all.begin(), all.end(), primary) == all.end()) {
        all.insert(all.begin(), primary);
    }
}

Config load_config(const std::optional<std::string>& yaml_path) {
    utils::LocalYamlStore store = load_config_store(yaml_path);

    Config cfg;
    cfg.listen_addr        = store.get_str("HTTP_HOST", cfg.listen_addr);
    cfg.listen_port        = static_cast<std::uint16_t>(store.get_int("HTTP_PORT", cfg.listen_port));
    cfg.uri_prefix         = normalize_uri_prefix(store.get_str("HTTP_URI_PREFIX", cfg.uri_prefix));
    // Negative is meaningless; treat it as "no bound" like 0 rather than letting
    // it wrap into a huge size_t at the router.
    cfg.max_request_body_bytes = store.get_int("HTTP_MAX_BODY_BYTES", cfg.max_request_body_bytes);
    if (cfg.max_request_body_bytes < 0) cfg.max_request_body_bytes = 0;
    cfg.public_base_url    = store.get_str("PUBLIC_BASE_URL", cfg.public_base_url);
    while (!cfg.public_base_url.empty() && cfg.public_base_url.back() == '/')
        cfg.public_base_url.pop_back();   // trim trailing '/' so links join cleanly

    cfg.openai.base_url     = store.get_str("OPENAI_BASE_URL",     cfg.openai.base_url);
    cfg.openai.realtime_url = store.get_str("OPENAI_REALTIME_URL", cfg.openai.realtime_url);
    cfg.openai.api_key      = store.get_str("OPENAI_API_KEY",      cfg.openai.api_key);

    cfg.gemini.base_url     = store.get_str("GEMINI_BASE_URL",     cfg.gemini.base_url);
    cfg.gemini.realtime_url = store.get_str("GEMINI_REALTIME_URL", cfg.gemini.realtime_url);
    cfg.gemini.api_key      = store.get_str("GEMINI_API_KEY",      cfg.gemini.api_key);
    if (cfg.gemini.api_key.empty()) {
        cfg.gemini.api_key = store.get_str("GOOGLE_API_KEY");
    }

    cfg.dashscope.base_url     = store.get_str("DASHSCOPE_BASE_URL",     cfg.dashscope.base_url);
    cfg.dashscope.realtime_url = store.get_str("DASHSCOPE_REALTIME_URL", cfg.dashscope.realtime_url);
    cfg.dashscope.api_key      = store.get_str("DASHSCOPE_API_KEY",      cfg.dashscope.api_key);

    cfg.deepseek.base_url     = store.get_str("DEEPSEEK_BASE_URL",     cfg.deepseek.base_url);
    cfg.deepseek.realtime_url = store.get_str("DEEPSEEK_REALTIME_URL", cfg.deepseek.realtime_url);
    cfg.deepseek.api_key      = store.get_str("DEEPSEEK_API_KEY",      cfg.deepseek.api_key);

    cfg.nvidia.base_url     = store.get_str("NVIDIA_BASE_URL",     cfg.nvidia.base_url);
    cfg.nvidia.api_key      = store.get_str("NVIDIA_API_KEY",      cfg.nvidia.api_key);

    cfg.zhipu.base_url     = store.get_str("ZHIPU_BASE_URL",     cfg.zhipu.base_url);
    cfg.zhipu.api_key      = store.get_str("ZHIPU_API_KEY",      cfg.zhipu.api_key);
    if (cfg.zhipu.api_key.empty()) {
        cfg.zhipu.api_key = store.get_str("GLM_API_KEY");
    }

    cfg.azure = load_azure_config(store);

    cfg.memory.provider = store.get_str("MEMORY_PROVIDER", cfg.memory.provider);
    cfg.memory.top_k    = static_cast<int>(store.get_int("MEMORY_TOP_K", cfg.memory.top_k));

    // VENDOR_TOKEN_ENCRYPTION_KEY: Fernet key list (last encrypts, all decrypt) for
    // per-user vendor OAuth tokens at rest. Same YAML-sequence-or-comma-scalar
    // parsing as FILE_ENCRYPTION_KEY above.
    cfg.vendor_redirect_uri = store.get_str("VENDOR_REDIRECT_URI", cfg.vendor_redirect_uri);

    cfg.vendor_token_encryption_keys = store.get_list("VENDOR_TOKEN_ENCRYPTION_KEY");
    if (cfg.vendor_token_encryption_keys.empty()) {
        const std::string raw = store.get_str("VENDOR_TOKEN_ENCRYPTION_KEY");
        std::size_t start = 0;
        while (!raw.empty() && start <= raw.size()) {
            std::size_t comma = raw.find(',', start);
            std::string tok = raw.substr(start, comma == std::string::npos ? std::string::npos : comma - start);
            std::size_t b = tok.find_first_not_of(" \t\r\n");
            std::size_t e = tok.find_last_not_of(" \t\r\n");
            if (b != std::string::npos) cfg.vendor_token_encryption_keys.push_back(tok.substr(b, e - b + 1));
            if (comma == std::string::npos) break;
            start = comma + 1;
        }
    }

    cfg.connect_timeout_ms = static_cast<int>(store.get_int("MIROBODY_CONNECT_TIMEOUT_MS", cfg.connect_timeout_ms));
    cfg.request_timeout_ms = static_cast<int>(store.get_int("MIROBODY_REQUEST_TIMEOUT_MS", cfg.request_timeout_ms));

    cfg.log_level = store.get_str("LOG_LEVEL", cfg.log_level);


    cfg.jwt.key             = store.get_str("JWT_KEY",          cfg.jwt.key);
    cfg.jwt.iss             = store.get_str("JWT_ISS",          cfg.jwt.iss);
    cfg.jwt.aud             = store.get_str("JWT_AUD",          cfg.jwt.aud);
    cfg.jwt.client_id       = store.get_str("JWT_CLIENT_ID",    cfg.jwt.client_id);
    cfg.jwt.scope           = store.get_str("JWT_SCOPE",        cfg.jwt.scope);
    cfg.jwt.salt            = store.get_str("JWT_SALT",         cfg.jwt.salt);
    cfg.jwt.private_key_pem = store.get_str("JWT_PRIVATE_KEY",  cfg.jwt.private_key_pem);
    cfg.jwt.public_key_pem  = store.get_str("JWT_PUBLIC_KEY",   cfg.jwt.public_key_pem);
    // Extra verify-only public keys for rotation: JWT_PUBLIC_KEY_2, _3, ...
    // Scanned contiguously (stop at the first absent suffix).
    for (int i = 2;; ++i) {
        const std::string pem = store.get_str("JWT_PUBLIC_KEY_" + std::to_string(i), "");
        if (pem.empty()) break;
        cfg.jwt.additional_public_key_pems.push_back(pem);
    }

    cfg.oauth.issuer        = store.get_str("OAUTH_ISSUER",        cfg.oauth.issuer);
    cfg.oauth.consent_path  = store.get_str("OAUTH_CONSENT_PATH",  cfg.oauth.consent_path);
    cfg.oauth.code_ttl      = store.get_int("OAUTH_CODE_TTL",      cfg.oauth.code_ttl);
    cfg.oauth.authz_ttl     = store.get_int("OAUTH_AUTHZ_TTL",     cfg.oauth.authz_ttl);
    cfg.oauth.client_ttl    = store.get_int("OAUTH_CLIENT_TTL",    cfg.oauth.client_ttl);
    cfg.oauth.scopes_supported = store.get_str("OAUTH_SCOPES",     cfg.oauth.scopes_supported);
    cfg.oauth.resource_path = store.get_str("OAUTH_RESOURCE_PATH", cfg.oauth.resource_path);
    cfg.oauth.allowed_redirect_hosts =
        store.get_str("OAUTH_ALLOWED_REDIRECT_HOSTS", cfg.oauth.allowed_redirect_hosts);

    cfg.smart_fhir.client_id     = store.get_str("SMART_FHIR_CLIENT_ID",     cfg.smart_fhir.client_id);
    cfg.smart_fhir.client_secret = store.get_str("SMART_FHIR_CLIENT_SECRET", cfg.smart_fhir.client_secret);
    cfg.smart_fhir.redirect_uri  = store.get_str("SMART_FHIR_REDIRECT_URI",  cfg.smart_fhir.redirect_uri);
    cfg.smart_fhir.scope         = store.get_str("SMART_FHIR_SCOPE",         cfg.smart_fhir.scope);
    cfg.smart_fhir.state_ttl     = store.get_int("SMART_FHIR_STATE_TTL",     cfg.smart_fhir.state_ttl);

    cfg.chat.rate_max_per_window = static_cast<int>(store.get_int("CHAT_RATE_MAX",        cfg.chat.rate_max_per_window));
    cfg.chat.rate_window_seconds = static_cast<int>(store.get_int("CHAT_RATE_WINDOW_SEC", cfg.chat.rate_window_seconds));
    cfg.chat.sse_heartbeat_seconds = static_cast<int>(
        store.get_int("CHAT_SSE_HEARTBEAT_SEC", cfg.chat.sse_heartbeat_seconds));
    if (cfg.chat.sse_heartbeat_seconds < 0) cfg.chat.sse_heartbeat_seconds = 0;   // negative == off

    cfg.circle.max_circles_per_user   = static_cast<int>(store.get_int("CIRCLE_MAX_PER_USER",      cfg.circle.max_circles_per_user));
    cfg.circle.max_members_per_circle = static_cast<int>(store.get_int("CIRCLE_MAX_MEMBERS",       cfg.circle.max_members_per_circle));
    cfg.circle.invite_max_per_window  = static_cast<int>(store.get_int("CIRCLE_INVITE_MAX",        cfg.circle.invite_max_per_window));
    cfg.circle.invite_window_seconds  = static_cast<int>(store.get_int("CIRCLE_INVITE_WINDOW_SEC", cfg.circle.invite_window_seconds));

    cfg.google_client_id    = store.get_str("GOOGLE_CLIENT_ID",     cfg.google_client_id);
    cfg.apple_client_id     = store.get_str("APPLE_CLIENT_ID",      cfg.apple_client_id);

    // FILE_ENCRYPTION_KEY is a list (last encrypts, all decrypt). A YAML
    // sequence parses via get_list; a plain or comma-separated scalar (the
    // env-var form) splits below.
    cfg.file_encryption_keys = store.get_list("FILE_ENCRYPTION_KEY");
    if (cfg.file_encryption_keys.empty()) {
        const std::string raw = store.get_str("FILE_ENCRYPTION_KEY");
        std::size_t start = 0;
        while (start <= raw.size()) {
            std::size_t comma = raw.find(',', start);
            std::string tok = raw.substr(start, comma == std::string::npos ? std::string::npos : comma - start);
            std::size_t b = tok.find_first_not_of(" \t\r\n");
            std::size_t e = tok.find_last_not_of(" \t\r\n");
            if (b != std::string::npos) cfg.file_encryption_keys.push_back(tok.substr(b, e - b + 1));
            if (comma == std::string::npos) break;
            start = comma + 1;
        }
    }
    // Vendor OAuth tokens are user data at rest, so when no dedicated
    // VENDOR_TOKEN_ENCRYPTION_KEY is set they fall back to the same at-rest key as
    // file uploads (FILE_ENCRYPTION_KEY) -- parsed just above, hence the fallback
    // sits here rather than with the vendor block. A deployment that already
    // configured file encryption thus gets per-user vendor-token encryption for
    // free (durable, works multi-instance); set VENDOR_TOKEN_ENCRYPTION_KEY only to
    // rotate it independently of the file key. With NEITHER key set there is no key,
    // and tokens are never persisted (see vendor_link.cpp).
    if (cfg.vendor_token_encryption_keys.empty()) {
        cfg.vendor_token_encryption_keys = cfg.file_encryption_keys;
    }
    cfg.file_key_seed       = store.get_str("FILE_KEY_SEED",        cfg.file_key_seed);
    cfg.file_url_prefix     = store.get_str("FILE_URL_PREFIX",      cfg.file_url_prefix);
    cfg.file_url_base       = store.get_str("FILE_URL_BASE",        cfg.file_url_base);
    // Normalize the mount path to a single leading '/', no trailing '/', so it
    // concatenates cleanly in signed_read_url() and matches the router mount.
    while (!cfg.file_url_prefix.empty() && cfg.file_url_prefix.back() == '/') cfg.file_url_prefix.pop_back();
    if (!cfg.file_url_prefix.empty() && cfg.file_url_prefix.front() != '/') {
        cfg.file_url_prefix.insert(cfg.file_url_prefix.begin(), '/');
    }
    while (!cfg.file_url_base.empty() && cfg.file_url_base.back() == '/') cfg.file_url_base.pop_back();

    cfg.wechat_appid      = store.get_str("WECHAT_APPID",       cfg.wechat_appid);
    cfg.wechat_secret     = store.get_str("WECHAT_SECRET",      cfg.wechat_secret);
    cfg.wechat_api_base   = store.get_str("WECHAT_API_BASE",    cfg.wechat_api_base);
    cfg.wechat_web_appid  = store.get_str("WECHAT_WEB_APPID",   cfg.wechat_web_appid);
    cfg.wechat_web_secret = store.get_str("WECHAT_WEB_SECRET",  cfg.wechat_web_secret);
    cfg.wechat_app_appid  = store.get_str("WECHAT_OPEN_APPID",  cfg.wechat_app_appid);
    cfg.wechat_app_secret = store.get_str("WECHAT_OPEN_SECRET", cfg.wechat_app_secret);

    cfg.github_client_id     = store.get_str("GITHUB_CLIENT_ID",     cfg.github_client_id);
    cfg.github_client_secret = store.get_str("GITHUB_CLIENT_SECRET", cfg.github_client_secret);
    cfg.github_oauth_base    = store.get_str("GITHUB_OAUTH_BASE",    cfg.github_oauth_base);
    cfg.github_api_base      = store.get_str("GITHUB_API_BASE",      cfg.github_api_base);


    cfg.email_predefine_codes = store.get_dict("EMAIL_PREDEFINE_CODES");
    cfg.email_predefine_domain_codes = store.get_dict("EMAIL_PREDEFINE_DOMAIN_CODES");

    cfg.email.smtp_host     = store.get_str("EMAIL_SMTP_HOST");
    cfg.email.smtp_port     = static_cast<int>(store.get_int("EMAIL_SMTP_PORT", 0));
    cfg.email.smtp_user     = store.get_str("EMAIL_SMTP_USER");
    cfg.email.smtp_pass     = store.get_str("EMAIL_SMTP_PASS");
    cfg.email.from_email    = store.get_str("EMAIL_FROM");
    cfg.email.from_name     = store.get_str("EMAIL_FROM_NAME");
    cfg.email.template_name = store.get_str("EMAIL_TEMPLATE");
    // FIREBASE_PROJECT_IDS is the full accepted set, primary first; FIREBASE_PROJECT_ID
    // is the single-project spelling. Either may be given: the list wins for the set,
    // and whichever is present supplies the primary (the one published to the web
    // client and named in the CSP). Parsing follows FILE_ENCRYPTION_KEY below -- a YAML
    // sequence via get_list, else a plain or comma-separated scalar.
    cfg.firebase_project_id = store.get_str("FIREBASE_PROJECT_ID", cfg.firebase_project_id);
    cfg.firebase_project_ids = store.get_list("FIREBASE_PROJECT_IDS");
    if (cfg.firebase_project_ids.empty()) {
        const std::string raw = store.get_str("FIREBASE_PROJECT_IDS");
        std::size_t start = 0;
        while (start <= raw.size()) {
            std::size_t comma = raw.find(',', start);
            std::string tok = raw.substr(start, comma == std::string::npos ? std::string::npos : comma - start);
            std::size_t b = tok.find_first_not_of(" \t\r\n");
            std::size_t e = tok.find_last_not_of(" \t\r\n");
            if (b != std::string::npos) cfg.firebase_project_ids.push_back(tok.substr(b, e - b + 1));
            if (comma == std::string::npos) break;
            start = comma + 1;
        }
    }
    reconcile_firebase_projects(cfg.firebase_project_id, cfg.firebase_project_ids);
    cfg.firebase_web_api_key        = store.get_str("FIREBASE_WEB_API_KEY", cfg.firebase_web_api_key);
    cfg.firebase_messaging_sender_id = store.get_str("FIREBASE_MESSAGING_SENDER_ID", cfg.firebase_messaging_sender_id);
    cfg.http_headers        = store.get_dict("HTTP_HEADERS");
    cfg.http_roots          = read_http_roots(store);
    cfg.sql_dir             = store.get_str("SQL_DIR", cfg.sql_dir);
    cfg.db_init_schema      = store.get_bool("DB_INIT_SCHEMA", cfg.db_init_schema);

    // SQLite, the one backend. Empty path opens an in-memory database, but the migration step and the server open separate
    // Database objects, so an in-memory DB would not be shared between them —
    // a file path is required for the two to see the same schema/data.
    cfg.sqlite.path         = store.get_str("SQLITE_PATH", cfg.sqlite.path);

    // Keys this server used to read and no longer does. Saying so once at startup
    // is the difference between a five-minute fix and an afternoon: a Vertex
    // deployment whose project now resolves to nothing looks exactly like one that
    // was never configured for Vertex at all, and the log otherwise says only that
    // it stayed on AI Studio.
    {
        struct Retired { const char* key; const char* use; };
        const Retired retired[] = {
            {"GCP_PROJECT",  "GOOGLE_CLOUD_PROJECT"},
            {"GCP_LOCATION", "GOOGLE_CLOUD_LOCATION"},
        };
        for (std::size_t i = 0; i < sizeof(retired) / sizeof(retired[0]); ++i) {
            if (!store.get_str(retired[i].key).empty()) {
                platform::log_warn("config: %s is set but is no longer read -- rename it to %s. "
                                   "Until then this setting has no effect.",
                                   retired[i].key, retired[i].use);
            }
        }
    }

    cfg.store = std::move(store);
    return cfg;
}

//------------------------------------------------------------------------------
// Process-wide config instance
//------------------------------------------------------------------------------

namespace {

// Held on the heap (not a function-local static) so init_config can replace it.
// Guarded by a mutex on (re)assignment; readers take the same lock only to
// obtain the reference, then read it lock-free thereafter, matching the rest of
// the codebase, which treats Config as read-only once startup has run.
std::mutex                g_config_mutex;
std::unique_ptr<Config>   g_config;

}   // namespace

Config& config() {
    std::lock_guard<std::mutex> lock(g_config_mutex);
    if (!g_config) {
        g_config.reset(new Config(load_config()));
    }
    return *g_config;
}

Config& init_config(const std::optional<std::string>& yaml_path) {
    std::lock_guard<std::mutex> lock(g_config_mutex);
    g_config.reset(new Config(load_config(yaml_path)));
    return *g_config;
}

//------------------------------------------------------------------------------

storage::LocalConfig Config::local_storage() const {
    storage::LocalConfig l;
    l.root       = store.get_str("LOCAL_STORAGE_DIR");
    l.prefix     = store.get_str("LOCAL_STORAGE_PREFIX", storage::default_prefix);
    l.url_prefix = store.get_str("LOCAL_STORAGE_URL_PREFIX");
    l.base_url   = store.get_str("LOCAL_STORAGE_BASE_URL");
    l.secret     = store.get_str("LOCAL_STORAGE_SECRET");

    // Normalize the serve path / key prefix to a single leading '/' and no
    // trailing '/' so it concatenates cleanly in public_url() and matches the
    // router mount.
    while (!l.url_prefix.empty() && l.url_prefix.back() == '/') l.url_prefix.pop_back();
    if (!l.url_prefix.empty() && l.url_prefix.front() != '/') {
        l.url_prefix.insert(l.url_prefix.begin(), '/');
    }
    return l;
}

//------------------------------------------------------------------------------

void Config::print() const {
    // Write to stderr, the same stream platform::log_* uses, so the startup dump
    // and the logger form one ordered stream. Sending it to stdout instead lets a
    // container log collector interleave the two pipes (stdout block-buffered,
    // stderr unbuffered), which scrambles startup output.
    std::FILE* out = stderr;
    std::fprintf(out, "\n");
    std::fprintf(out, "Configuration\n");
    std::fprintf(out, "------------------------------------------------------------\n");
    std::fprintf(out, "  listen          : %s:%u\n", listen_addr.c_str(), static_cast<unsigned>(listen_port));
    std::fprintf(out, "  uri_prefix      : %s\n",    uri_prefix.empty() ? "<root>" : uri_prefix.c_str());
    std::fprintf(out, "  log_level       : %s\n",    log_level.c_str());
    std::fprintf(out, "  connect_timeout : %d ms\n", connect_timeout_ms);
    std::fprintf(out, "  request_timeout : %d ms\n", request_timeout_ms);
    if (http_roots.empty()) {
        std::fprintf(out, "  http_root       : %s\n", "<not set>");
    } else {
        for (const auto& mount : http_roots) {
            std::fprintf(out, "  http_root       : %s -> %s\n",
                         mount.first.c_str(), mount.second.c_str());
        }
    }
    std::fprintf(out, "  sql_dir         : %s\n",    sql_dir.empty() ? "<not set>" : sql_dir.c_str());
    std::fprintf(out, "  db_init_schema  : %s\n",    db_init_schema ? "true" : "false");

    // LLM providers are only shown when their API key is set, mirroring the
    // jwt section below: an unconfigured provider is noise.
    if (!openai.api_key.empty()) {
        std::fprintf(out, "\n");
        std::fprintf(out, "  openai\n");
        std::fprintf(out, "    base_url      : %s\n", openai.base_url.c_str());
        std::fprintf(out, "    realtime_url  : %s\n", openai.realtime_url.c_str());
        std::fprintf(out, "    api_key       : %s\n", mask(openai.api_key).c_str());
    }
    if (!gemini.api_key.empty()) {
        std::fprintf(out, "\n");
        std::fprintf(out, "  gemini\n");
        std::fprintf(out, "    base_url      : %s\n", gemini.base_url.c_str());
        std::fprintf(out, "    realtime_url  : %s\n", gemini.realtime_url.c_str());
        std::fprintf(out, "    api_key       : %s\n", mask(gemini.api_key).c_str());
    }
    if (!azure.endpoint.empty() || !azure.key.empty()) {
        std::fprintf(out, "\n");
        std::fprintf(out, "  azure\n");
        if (!azure.endpoint.empty())   std::fprintf(out, "    endpoint      : %s\n", azure.endpoint.c_str());
        if (!azure.deployment.empty()) std::fprintf(out, "    deployment    : %s\n", azure.deployment.c_str());
        std::fprintf(out, "    api_version   : %s\n", azure.api_version.c_str());
        std::fprintf(out, "    key           : %s\n", mask(azure.key).c_str());
    }
    if (!dashscope.api_key.empty()) {
        std::fprintf(out, "\n");
        std::fprintf(out, "  dashscope\n");
        std::fprintf(out, "    base_url      : %s\n", dashscope.base_url.c_str());
        if (!dashscope.realtime_url.empty())
            std::fprintf(out, "    realtime_url  : %s\n", dashscope.realtime_url.c_str());
        std::fprintf(out, "    api_key       : %s\n", mask(dashscope.api_key).c_str());
    }
    if (!deepseek.api_key.empty()) {
        std::fprintf(out, "\n");
        std::fprintf(out, "  deepseek\n");
        std::fprintf(out, "    base_url      : %s\n", deepseek.base_url.c_str());
        if (!deepseek.realtime_url.empty())
            std::fprintf(out, "    realtime_url  : %s\n", deepseek.realtime_url.c_str());
        std::fprintf(out, "    api_key       : %s\n", mask(deepseek.api_key).c_str());
    }
    if (!nvidia.api_key.empty()) {
        std::fprintf(out, "\n");
        std::fprintf(out, "  nvidia\n");
        std::fprintf(out, "    base_url      : %s\n", nvidia.base_url.c_str());
        std::fprintf(out, "    api_key       : %s\n", mask(nvidia.api_key).c_str());
    }
    if (!zhipu.api_key.empty()) {
        std::fprintf(out, "\n");
        std::fprintf(out, "  zhipu\n");
        std::fprintf(out, "    base_url      : %s\n", zhipu.base_url.c_str());
        std::fprintf(out, "    api_key       : %s\n", mask(zhipu.api_key).c_str());
    }

    if (!sqlite.path.empty()) {
        std::fprintf(out, "\n");
        std::fprintf(out, "  sqlite\n");
        std::fprintf(out, "    path          : %s\n", sqlite.path.c_str());
    }
    const storage::LocalConfig localc = local_storage();
    if (localc.configured()) {
        std::fprintf(out, "\n");
        std::fprintf(out, "  local_storage\n");
        std::fprintf(out, "    root          : %s\n", localc.root.c_str());
        if (!localc.url_prefix.empty()) std::fprintf(out, "    url_prefix    : %s\n", localc.url_prefix.c_str());
        if (!localc.base_url.empty()) std::fprintf(out, "    base_url      : %s\n", localc.base_url.c_str());
        if (!localc.secret.empty())   std::fprintf(out, "    secret        : %s\n", mask(localc.secret).c_str());
    }

    if (!jwt.key.empty()) {
        std::fprintf(out, "\n");
        std::fprintf(out, "  jwt\n");
        std::fprintf(out, "    key           : %s\n", mask(jwt.key).c_str());
        // iss / aud / client_id / scope have no defaults (empty unless set in
        // config); server.cpp only applies the non-empty ones, so skip blanks.
        if (!jwt.iss.empty())       std::fprintf(out, "    iss           : %s\n", jwt.iss.c_str());
        if (!jwt.aud.empty())       std::fprintf(out, "    aud           : %s\n", jwt.aud.c_str());
        if (!jwt.client_id.empty()) std::fprintf(out, "    client_id     : %s\n", jwt.client_id.c_str());
        if (!jwt.scope.empty())     std::fprintf(out, "    scope         : %s\n", jwt.scope.c_str());
    }

    if (!google_client_id.empty()) {
        std::fprintf(out, "\n");
        std::fprintf(out, "  google\n");
        std::fprintf(out, "    client_id     : %s\n", google_client_id.c_str());
    }

    if (!apple_client_id.empty()) {
        std::fprintf(out, "\n");
        std::fprintf(out, "  apple\n");
        std::fprintf(out, "    client_id     : %s\n", apple_client_id.c_str());
    }

    if (!firebase_project_id.empty()) {
        std::fprintf(out, "\n");
        std::fprintf(out, "  firebase\n");
        std::fprintf(out, "    project_id    : %s\n", firebase_project_id.c_str());
        // Only worth a line when there is more than the primary; otherwise it
        // would just repeat the line above.
        if (firebase_project_ids.size() > 1) {
            std::string accepted;
            for (std::size_t i = 0; i < firebase_project_ids.size(); ++i) {
                if (i) accepted += ", ";
                accepted += firebase_project_ids[i];
            }
            std::fprintf(out, "    accepting     : %s\n", accepted.c_str());
        }
    }

    if (!wechat_appid.empty() || !wechat_web_appid.empty() || !wechat_app_appid.empty()) {
        std::fprintf(out, "\n");
        std::fprintf(out, "  wechat\n");
        if (!wechat_appid.empty()) {
            std::fprintf(out, "    appid         : %s\n", wechat_appid.c_str());
            std::fprintf(out, "    secret        : %s\n", mask(wechat_secret).c_str());
        }
        if (!wechat_web_appid.empty()) {
            std::fprintf(out, "    web_appid     : %s\n", wechat_web_appid.c_str());
            std::fprintf(out, "    web_secret    : %s\n", mask(wechat_web_secret).c_str());
        }
        if (!wechat_app_appid.empty()) {
            std::fprintf(out, "    app_appid     : %s\n", wechat_app_appid.c_str());
            std::fprintf(out, "    app_secret    : %s\n", mask(wechat_app_secret).c_str());
        }
    }

    if (!github_client_id.empty()) {
        std::fprintf(out, "\n");
        std::fprintf(out, "  github\n");
        std::fprintf(out, "    client_id     : %s\n", github_client_id.c_str());
        std::fprintf(out, "    client_secret : %s\n", mask(github_client_secret).c_str());
    }

    // Email transport: only shown when an SMTP host is set. smtp_pass doubles
    // as the Mandrill API key, so it is masked either way.
    if (!email.smtp_host.empty()) {
        std::fprintf(out, "\n");
        std::fprintf(out, "  email\n");
        std::fprintf(out, "    smtp_host     : %s:%d\n", email.smtp_host.c_str(), email.smtp_port);
        if (!email.smtp_user.empty())     std::fprintf(out, "    smtp_user     : %s\n", email.smtp_user.c_str());
        std::fprintf(out, "    smtp_pass     : %s\n", mask(email.smtp_pass).c_str());
        if (!email.from_email.empty())    std::fprintf(out, "    from_email    : %s\n", email.from_email.c_str());
        if (!email.from_name.empty())     std::fprintf(out, "    from_name     : %s\n", email.from_name.c_str());
        if (!email.template_name.empty()) std::fprintf(out, "    template      : %s\n", email.template_name.c_str());
    }

    // Demo/test logins: each email may sign in with its mapped code instead of
    // an emailed one. Codes are non-secret test credentials, so print verbatim.
    if (!email_predefine_codes.empty()) {
        std::fprintf(out, "\n");
        std::fprintf(out, "  email_predefine_codes\n");
        for (const auto& kv : email_predefine_codes) {
            std::fprintf(out, "    %-30s: %s\n", kv.first.c_str(), kv.second.c_str());
        }
    }

    // Domain-wide demo logins: any address in a listed domain logs in with the
    // mapped code. Non-secret test credentials, so printed verbatim.
    if (!email_predefine_domain_codes.empty()) {
        std::fprintf(out, "\n");
        std::fprintf(out, "  email_predefine_domain_codes\n");
        for (const auto& kv : email_predefine_domain_codes) {
            std::fprintf(out, "    %-30s: %s\n", kv.first.c_str(), kv.second.c_str());
        }
    }

    std::fprintf(out, "------------------------------------------------------------\n");
    std::fflush(out);
}

}

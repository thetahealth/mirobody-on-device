#include "user/service.hpp"

#include "client/http_client.hpp"
#include "database/enums.hpp"
#include "platform/clock.hpp"   // now_unix_ms
#include "platform/log.hpp"     // log_warn

#include <rapidjson/document.h>
#include <rapidjson/stringbuffer.h>
#include <rapidjson/writer.h>

#include <cctype>
#include <cstdint>
#include <exception>
#include <string>
#include <unordered_map>
#include <vector>

namespace mirobody { namespace user {

namespace {

//------------------------------------------------------------------------------
// Helpers
//------------------------------------------------------------------------------

std::string to_lower_trim(const std::string& s) {
    std::size_t b = 0, e = s.size();
    while (b < e && std::isspace(static_cast<unsigned char>(s[b]))) ++b;
    while (e > b && std::isspace(static_cast<unsigned char>(s[e - 1]))) --e;
    std::string out;
    out.reserve(e - b);
    for (std::size_t i = b; i < e; ++i) {
        out.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(s[i]))));
    }
    return out;
}

// Read a string member from `doc`, accepting a JSON string or (for codes sent
// as a number) an integer. Returns empty when absent or another type.
std::string get_field(const rapidjson::Document& doc, const char* key) {
    if (!doc.HasMember(key)) return std::string();
    const rapidjson::Value& v = doc[key];
    if (v.IsString()) return std::string(v.GetString(), v.GetStringLength());
    if (v.IsInt64())  return std::to_string(v.GetInt64());
    if (v.IsInt())    return std::to_string(v.GetInt());
    return std::string();
}

// True only when the verified claims assert the email is verified. The
// `email_verified` claim is a JSON bool in Firebase ID tokens and a string
// ("true"/"false") in Apple's, so accept either form; a missing or false claim
// counts as NOT verified. This gate matters because a broker like Firebase lets
// anyone sign up with an arbitrary, unverified address -- trusting it would take
// over the existing account keyed by that email.
bool claims_email_verified(const rapidjson::Document& doc) {
    if (!doc.IsObject() || !doc.HasMember("email_verified")) return false;
    const rapidjson::Value& v = doc["email_verified"];
    if (v.IsBool())   return v.GetBool();
    if (v.IsString()) return std::string(v.GetString(), v.GetStringLength()) == "true";
    return false;
}

// Map a Firebase token's nested firebase.sign_in_provider claim to a LoginMethod
// integer. Firebase brokers several providers; the claim is e.g. "google.com",
// "github.com", "twitter.com", "apple.com", "password". Anything else -> Unknown.
int firebase_login_method(const rapidjson::Document& doc) {
    using database::LoginMethod;
    if (!doc.IsObject() || !doc.HasMember("firebase") || !doc["firebase"].IsObject()) {
        return static_cast<int>(LoginMethod::Unknown);
    }
    const rapidjson::Value& fb = doc["firebase"];
    if (!fb.HasMember("sign_in_provider") || !fb["sign_in_provider"].IsString()) {
        return static_cast<int>(LoginMethod::Unknown);
    }
    const std::string p = fb["sign_in_provider"].GetString();
    if (p == "google.com")  return static_cast<int>(LoginMethod::Google);
    if (p == "github.com")  return static_cast<int>(LoginMethod::GitHub);
    if (p == "twitter.com") return static_cast<int>(LoginMethod::X);
    if (p == "apple.com")   return static_cast<int>(LoginMethod::Apple);
    if (p == "password")    return static_cast<int>(LoginMethod::Email);
    return static_cast<int>(LoginMethod::Unknown);
}

// Build the email-validator options from configuration. Falls back smtp_user to
// from_email when unset, matching the Python service wiring.
EmailValidatorOptions email_options(const Config& cfg) {
    EmailValidatorOptions opts;
    opts.smtp_host        = cfg.email.smtp_host;
    opts.smtp_port        = cfg.email.smtp_port;
    opts.smtp_user        = cfg.email.smtp_user.empty() ? cfg.email.from_email : cfg.email.smtp_user;
    opts.smtp_pass        = cfg.email.smtp_pass;
    opts.mandrill_api_key = cfg.email.smtp_pass;   // smtp_pass doubles as the API key
    opts.mandrill_template = cfg.email.template_name;
    opts.from_email       = cfg.email.from_email;
    opts.from_name        = cfg.email.from_name;
    opts.predefined_codes = cfg.email_predefine_codes;
    opts.predefined_domain_codes = cfg.email_predefine_domain_codes;
    return opts;
}

// Serialize the Firebase web-app config: the GET /firebase/verify data object,
// also spliced into GET /auth/providers under "google". Empty when Google
// sign-in isn't configured (no api key / project id), which is how both routes
// signal "unavailable" so the client hides the button. authDomain is
// omitted (the client derives "<projectId>.firebaseapp.com") and so is appId
// (not needed for Auth).
std::string build_firebase_web_config(const Config& cfg) {
    if (cfg.firebase_web_api_key.empty() || cfg.firebase_project_id.empty()) {
        return std::string();
    }

    rapidjson::StringBuffer buf;
    rapidjson::Writer<rapidjson::StringBuffer> w(buf);
    auto str = [&](const char* key, const std::string& v) {
        w.Key(key);
        w.String(v.c_str(), static_cast<rapidjson::SizeType>(v.size()));
    };
    w.StartObject();
    str("apiKey",            cfg.firebase_web_api_key);
    str("projectId",         cfg.firebase_project_id);
    str("messagingSenderId", cfg.firebase_messaging_sender_id);
    w.EndObject();
    return std::string(buf.GetString(), buf.GetSize());
}

// Serialize the Apple web config: the GET /apple/verify data object, also spliced
// into GET /auth/providers. Empty when Apple sign-in isn't configured (no
// Services ID), which is how both routes signal "unavailable". Only the clientId is exposed --
// the client needs it to initialize the Apple JS SDK.
std::string build_apple_web_config(const Config& cfg) {
    if (cfg.apple_client_id.empty()) {
        return std::string();
    }
    rapidjson::StringBuffer buf;
    rapidjson::Writer<rapidjson::StringBuffer> w(buf);
    w.StartObject();
    w.Key("clientId");
    w.String(cfg.apple_client_id.c_str(),
             static_cast<rapidjson::SizeType>(cfg.apple_client_id.size()));
    w.EndObject();
    return std::string(buf.GetString(), buf.GetSize());
}

// Serialize the WeChat web config: the GET /wechat/verify data object, also
// spliced into GET /auth/providers. Empty when web sign-in isn't configured (no
// appid/secret), which is how both routes signal "unavailable". Only the appid is exposed --
// the client needs it to build the qrconnect / oauth2 authorize URL; the secret
// stays server-side for the code exchange.
std::string build_wechat_web_config(const std::string& appid, const std::string& secret) {
    if (appid.empty() || secret.empty()) {
        return std::string();
    }
    rapidjson::StringBuffer buf;
    rapidjson::Writer<rapidjson::StringBuffer> w(buf);
    w.StartObject();
    w.Key("appid");
    w.String(appid.c_str(), static_cast<rapidjson::SizeType>(appid.size()));
    w.EndObject();
    return std::string(buf.GetString(), buf.GetSize());
}

// Serialize the GitHub web config: the GET /github/verify data object, also
// spliced into GET /auth/providers. Empty when sign-in isn't configured (no
// client_id/secret), which is how both routes signal "unavailable". Only the clientId is exposed --
// the client needs it to build the authorize URL; the secret stays server-side
// for the code exchange.
std::string build_github_web_config(const std::string& client_id, const std::string& secret) {
    if (client_id.empty() || secret.empty()) {
        return std::string();
    }
    rapidjson::StringBuffer buf;
    rapidjson::Writer<rapidjson::StringBuffer> w(buf);
    w.StartObject();
    w.Key("clientId");
    w.String(client_id.c_str(), static_cast<rapidjson::SizeType>(client_id.size()));
    w.EndObject();
    return std::string(buf.GetString(), buf.GetSize());
}

// Serialize the whole sign-in capability set as the GET /auth/providers data
// object: which federated providers this deployment has configured, and the
// public config each one needs, in a single document.
//
// It exists because the per-provider GET routes above are a side channel bolted
// onto their POST verify endpoints: a client that wants to know which buttons to
// show has to issue four requests and read four "is this an error or just
// unconfigured" replies. That is tolerable in a browser and wasteful on a phone.
//
// The already-built config strings are spliced in verbatim (RawValue), so this
// adds no second serializer to drift from the first four.
//
// A provider appears with "enabled": false only when the server actually gates
// it. One the server has no opinion about is OMITTED entirely -- that is how a
// client tells "this deployment turned WeChat off" apart from "this build knows
// nothing about X either way", which a blanket false would flatten.
std::string build_auth_providers(const Config& cfg,
                                 const std::string& firebase_web_config,
                                 const std::string& apple_web_config,
                                 const std::string& wechat_web_config,
                                 const std::string& github_web_config,
                                 bool firebase_verify_configured,
                                 bool wechat_app_configured) {
    rapidjson::StringBuffer buf;
    rapidjson::Writer<rapidjson::StringBuffer> w(buf);

    // One provider entry:
    //   "enabled"    the WEB flow works -- the browser has what it needs, which for
    //                most providers means we hold public config to hand it.
    //   "config"     that public config, present only when enabled.
    //   "appEnabled" the NATIVE flow works. Only emitted where the two can differ;
    //                see the two call sites below for why they can.
    //
    // The split exists because "configured" is not one question. A browser needs
    // credentials we publish to it; a native app carries its own and only needs the
    // server able to complete the exchange. Collapsing them would have this document
    // tell a phone to hide a button that works.
    auto provider = [&](const char* name, const std::string& config_json,
                        const bool* app_enabled) {
        w.Key(name);
        w.StartObject();
        w.Key("enabled");
        w.Bool(!config_json.empty());
        if (!config_json.empty()) {
            w.Key("config");
            w.RawValue(config_json.data(), config_json.size(), rapidjson::kObjectType);
        }
        if (app_enabled) {
            w.Key("appEnabled");
            w.Bool(*app_enabled);
        }
        w.EndObject();
    };

    w.StartObject();

    // Google (and X, which the clients broker through the same Firebase project).
    // The web button needs FIREBASE_WEB_API_KEY to initialize the JS SDK; verifying
    // an ID token needs only FIREBASE_PROJECT_ID (see server.cpp, where the
    // validator is constructed from it alone). A server with the project id and no
    // web key verifies native sign-ins perfectly well, so the apps read appEnabled.
    provider("google", firebase_web_config, &firebase_verify_configured);

    provider("apple",  apple_web_config, nullptr);
    provider("github", github_web_config, nullptr);

    // WeChat has two independent registrations and a server may hold either: the
    // browser drives an Open Platform *website* app (qrconnect / oauth2,
    // WECHAT_WEB_*), the native apps drive a *mobile application* through the
    // OpenSDK (WECHAT_OPEN_*). appEnabled is a bare flag -- the app already carries
    // its own appid and only needs to know the server can complete the exchange.
    provider("wechat", wechat_web_config, &wechat_app_configured);

    // Tanka QR sign-in was web-only and left with the web client; the key stays
    // (always disabled) so clients that read it keep parsing the same shape.
    w.Key("tanka");
    w.StartObject();
    w.Key("enabled");
    w.Bool(false);
    w.EndObject();

    w.EndObject();
    return std::string(buf.GetString(), buf.GetSize());
}

// Percent-encode `s` into an application/x-www-form-urlencoded component
// (RFC 3986 unreserved chars pass through). Used to build the GitHub token
// exchange body from attacker-supplied input (the OAuth code) and our own
// credentials alike.
std::string url_encode(const std::string& s) {
    std::string out;
    out.reserve(s.size() * 3);
    for (unsigned char c : s) {
        if (std::isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') {
            out.push_back(static_cast<char>(c));
        } else {
            static const char* hex = "0123456789ABCDEF";
            out.push_back('%');
            out.push_back(hex[c >> 4]);
            out.push_back(hex[c & 0x0F]);
        }
    }
    return out;
}

// Only the audit write below needs these; the legacy schema has no log table.

// Whether `s` is a bare dotted-quad IPv4 literal. Rejects leading zeros (which
// some parsers read as octal) so only one spelling of an address is stored.
bool is_ipv4_literal(const std::string& s) {
    int groups = 0;
    std::size_t i = 0;
    while (i < s.size()) {
        const std::size_t start = i;
        int value = 0;
        while (i < s.size() && std::isdigit(static_cast<unsigned char>(s[i]))) {
            value = value * 10 + (s[i] - '0');
            if (++i - start > 3) return false;
        }
        if (i == start || value > 255) return false;
        if (s[start] == '0' && i - start > 1) return false;
        if (++groups > 4) return false;
        if (i == s.size()) break;
        if (s[i] != '.') return false;
        if (++i == s.size()) return false;          // trailing '.'
    }
    return groups == 4;
}

// Whether `s` is a bare IPv6 literal, including the compressed "::" form and an
// IPv4-mapped tail (`::ffff:203.0.113.7`). A zone id (`%eth0`) or a "/masklen"
// suffix falls out as invalid, which is what we want: pg's INET rejects the
// former and would silently widen the row's meaning on the latter.
bool is_ipv6_literal(const std::string& s) {
    if (s.size() < 2 || s.size() > 45 || s.find(':') == std::string::npos) return false;
    // A lone leading/trailing ':' is only legal as half of an elision.
    if (s[0] == ':' && s.compare(0, 2, "::") != 0) return false;
    if (s[s.size() - 1] == ':' && s.compare(s.size() - 2, 2, "::") != 0) return false;

    std::vector<std::string> parts;                 // an empty part marks the elision
    for (std::size_t start = 0;;) {
        const std::size_t colon = s.find(':', start);
        parts.push_back(s.substr(start, colon == std::string::npos ? colon : colon - start));
        if (colon == std::string::npos) break;
        start = colon + 1;
    }
    // An edge "::" splits into two empty parts; drop one so a single empty part
    // means "elision here" wherever it sits.
    if (parts.size() >= 2 && parts[0].empty() && parts[1].empty()) parts.erase(parts.begin());
    if (parts.size() >= 2 && parts.back().empty() && parts[parts.size() - 2].empty()) parts.pop_back();

    int groups = 0, elisions = 0;                   // an embedded IPv4 fills two groups
    for (std::size_t k = 0; k < parts.size(); ++k) {
        const std::string& g = parts[k];
        if (g.empty()) { ++elisions; continue; }
        if (g.find('.') != std::string::npos) {
            if (k + 1 != parts.size() || !is_ipv4_literal(g)) return false;
            groups += 2;
            continue;
        }
        if (g.size() > 4) return false;
        for (std::size_t j = 0; j < g.size(); ++j) {
            if (!std::isxdigit(static_cast<unsigned char>(g[j]))) return false;
        }
        ++groups;
    }
    if (elisions > 1) return false;
    return elisions == 1 ? groups < 8 : groups == 8;
}


}  // namespace

//------------------------------------------------------------------------------
// Construction
//------------------------------------------------------------------------------

UserService::UserService(server::Router& router,
                         const Config& cfg,
                         database::Database& db,
                         cache::Cache& cache,
                         const jwt::Jwt& jwt,
                         jwt::FirebaseTokenValidator* firebase,
                         jwt::AppleTokenValidator* apple)
    : db_(db), cache_(cache), jwt_(jwt), firebase_(firebase), apple_(apple),
      email_validator_(create_email_validator(email_options(cfg), cache_)),
      firebase_web_config_(build_firebase_web_config(cfg)),
      apple_web_config_(build_apple_web_config(cfg)),
      wechat_appid_(cfg.wechat_appid),
      wechat_secret_(cfg.wechat_secret),
      wechat_api_base_(cfg.wechat_api_base),
      // Mobile-app credentials fall back to the web, then the Mini Program ones.
      wechat_app_appid_(!cfg.wechat_app_appid.empty() ? cfg.wechat_app_appid
                        : !cfg.wechat_web_appid.empty() ? cfg.wechat_web_appid
                        : cfg.wechat_appid),
      wechat_app_secret_(!cfg.wechat_app_secret.empty() ? cfg.wechat_app_secret
                         : !cfg.wechat_web_secret.empty() ? cfg.wechat_web_secret
                         : cfg.wechat_secret),
      // Web sign-in credentials fall back to the Mini Program ones when no
      // separate WECHAT_WEB_* pair is configured.
      wechat_web_appid_(cfg.wechat_web_appid.empty() ? cfg.wechat_appid : cfg.wechat_web_appid),
      wechat_web_secret_(cfg.wechat_web_secret.empty() ? cfg.wechat_secret : cfg.wechat_web_secret),
      wechat_web_config_(build_wechat_web_config(wechat_web_appid_, wechat_web_secret_)),
      github_client_id_(cfg.github_client_id),
      github_client_secret_(cfg.github_client_secret),
      github_oauth_base_(cfg.github_oauth_base),
      github_api_base_(cfg.github_api_base),
      github_web_config_(build_github_web_config(cfg.github_client_id, cfg.github_client_secret)),
      // Composed last: it splices the four config strings above, so they must
      // already be initialized (member init runs in declaration order).
      auth_providers_(build_auth_providers(cfg, firebase_web_config_, apple_web_config_,
                                           wechat_web_config_, github_web_config_,
                                           // "can we verify a Firebase token" is the
                                           // accepted SET, not the primary alone.
                                           !cfg.firebase_project_ids.empty(),
                                           !wechat_app_appid_.empty() && !wechat_app_secret_.empty())) {
    register_routes(router);
}

//------------------------------------------------------------------------------
// User lookup / token minting
//------------------------------------------------------------------------------

std::int64_t UserService::add_or_get_user(const std::string& email, std::string* err) {
    const std::string lower = to_lower_trim(email);
    if (lower.empty()) {
        *err = "Invalid email.";
        return 0;
    }
    // Default display name: the local-part of the address.
    std::string name = lower.substr(0, lower.find('@'));

    // The user table differs by schema. The legacy schema is `health_app_user`
    // with an `is_del` flag; the current schema is `users` with a `deleted_at`
    // timestamp. Other backends are not built yet -- they reuse the `#else`
    // (new-schema) SQL; a `last_insert_id` branch is only needed once a backend
    // without RETURNING (e.g. MySQL) is introduced. Bound params are identical
    // for both arms ({lower} and {lower, name}) -- the legacy FALSE is a literal.
    static const char* const kSelectByEmail =
        "SELECT id FROM users WHERE email=? AND deleted_at IS NULL;";
    static const char* const kInsertUser =
        "INSERT INTO users (email,name,created_at) VALUES (?,?,?) RETURNING id;";

    // created_at is unix ms the app stamps (the modern schema has no DB default);
    // updated_at is left NULL until the row is later changed. The legacy
    // health_app_user keeps its own DB-side timestamps.
    const std::vector<database::Value> insert_params = {lower, name, platform::now_unix_ms()};

    try {
        database::Result sel = db_.execute(kSelectByEmail, {lower});
        if (!sel.rows.empty() && !sel.rows[0].empty()) {
            return sel.rows[0][0].as_int();
        }

        database::Result ins = db_.execute(kInsertUser, insert_params);
        if (!ins.rows.empty() && !ins.rows[0].empty()) {
            return ins.rows[0][0].as_int();
        }
    } catch (const std::exception& e) {
        *err = e.what();
        return 0;
    }

    *err = "Not found.";
    return 0;
}

std::int64_t UserService::add_or_get_wechat_user(const std::string& openid,
                                                 const std::string& unionid,
                                                 std::string* err) {
    if (openid.empty()) {
        *err = "Missing openid.";
        return 0;
    }
    // Current schema: the WeChat identity lives in user_identities, keyed by
    // (login_method, provider_uid). Prefer the unionid as the provider id so the
    // same person across a WeChat open-platform's apps maps to one account.
    const int kWeChat = static_cast<int>(database::LoginMethod::WeChat);
    const std::string provider_uid = unionid.empty() ? openid : unionid;
    try {
        database::Result sel = db_.execute(
            "SELECT user_id FROM user_identities WHERE login_method=? AND provider_uid=?;",
            {kWeChat, provider_uid});
        if (!sel.rows.empty() && !sel.rows[0].empty()) {
            return sel.rows[0][0].as_int();
        }

        // New identity: create the user and link it in one transaction so a
        // failure never leaves a users row with no identity.
        const std::int64_t now = platform::now_unix_ms();
        database::Transaction tx = db_.begin();
        database::Result ins = tx.execute(
            "INSERT INTO users (name, created_at) VALUES (?, ?) RETURNING id;",
            {std::string("WeChat User"), now});
        if (ins.rows.empty() || ins.rows[0].empty()) {
            *err = "Failed to create user.";   // tx rolls back on scope exit
            return 0;
        }
        const std::int64_t user_id = ins.rows[0][0].as_int();
        tx.execute(
            "INSERT INTO user_identities (user_id, login_method, provider_uid, created_at) "
            "VALUES (?, ?, ?, ?);",
            {user_id, kWeChat, provider_uid, now});
        tx.commit();
        return user_id;
    } catch (const std::exception& e) {
        *err = e.what();
        return 0;
    }

    *err = "Not found.";
    return 0;
}

std::int64_t UserService::add_or_get_identity_user(int login_method,
                                                   const std::string& provider_uid,
                                                   const std::string& email,
                                                   std::string* err) {
    if (provider_uid.empty()) {
        *err = "Missing provider subject.";
        return 0;
    }
    // The provider-reported email is recorded on the identity for audit but is
    // NOT trusted as the account email (it is absent or unverified here), so the
    // users row is created with a NULL email.
    const database::Value email_value =
        email.empty() ? database::Value(nullptr) : database::Value(email);
    try {
        database::Result sel = db_.execute(
            "SELECT user_id FROM user_identities WHERE login_method=? AND provider_uid=?;",
            {login_method, provider_uid});
        if (!sel.rows.empty() && !sel.rows[0].empty()) {
            return sel.rows[0][0].as_int();
        }

        // New identity: create the user and link it in one transaction so a
        // failure never leaves a users row with no identity.
        const std::int64_t now = platform::now_unix_ms();
        database::Transaction tx = db_.begin();
        database::Result ins = tx.execute(
            "INSERT INTO users (name, created_at) VALUES ('', ?) RETURNING id;",
            {now});
        if (ins.rows.empty() || ins.rows[0].empty()) {
            *err = "Failed to create user.";   // tx rolls back on scope exit
            return 0;
        }
        const std::int64_t user_id = ins.rows[0][0].as_int();
        tx.execute(
            "INSERT INTO user_identities (user_id, login_method, provider_uid, email, created_at) "
            "VALUES (?, ?, ?, ?, ?);",
            {user_id, login_method, provider_uid, email_value, now});
        tx.commit();
        return user_id;
    } catch (const std::exception& e) {
        *err = e.what();
        return 0;
    }
}

bool UserService::bind_email(std::int64_t user_id, const std::string& email, std::string* err) {
    const std::string lower = to_lower_trim(email);
    if (lower.empty()) {
        *err = "Invalid email.";
        return false;
    }
    try {
        database::Result sel = db_.execute(
            "SELECT id FROM users WHERE email=? AND deleted_at IS NULL;", {lower});
        if (!sel.rows.empty() && !sel.rows[0].empty() && sel.rows[0][0].as_int() != user_id) {
            *err = "Email already in use.";
            return false;
        }
        db_.execute("UPDATE users SET email=?, updated_at=? WHERE id=?;",
                    {lower, platform::now_unix_ms(), user_id});
        return true;
    } catch (const std::exception& e) {
        // The active-email unique index is the backstop against a race between the
        // check and the update; its violation surfaces here.
        *err = e.what();
        return false;
    }
}

void UserService::generate_auth_response(server::Response& res,
                                         std::int64_t user_id, const std::string& email) {
    const std::string lower = to_lower_trim(email);

    // generate() encodes the row id into the token subject itself (keyed by the
    // JWT salt); we just pass the raw user_id.
    // Access token: carries the user's email and an access-token marker.
    std::unordered_map<std::string, std::string> access_claims;
    access_claims["email"]      = lower;
    access_claims["token_type"] = "oauth_access_token";
    const std::string access_token = jwt_.generate(user_id, access_claims, 0);

    // Refresh token: longer-lived (2x), marked as a refresh token. Matches the
    // Python generate_tokens (refresh_expires = get_expires_in() * 2).
    std::unordered_map<std::string, std::string> refresh_claims;
    refresh_claims["token_type"] = "oauth_refresh_token";
    const std::string refresh_token = jwt_.generate(user_id, refresh_claims, jwt_.expires_in() * 2);

    rapidjson::StringBuffer data;
    rapidjson::Writer<rapidjson::StringBuffer> w(data);
    w.StartObject();
    w.Key("access_token");  w.String(access_token.c_str(), static_cast<rapidjson::SizeType>(access_token.size()));
    w.Key("token_type");    w.String("Bearer");
    w.Key("expires_in");    w.Int64(jwt_.expires_in());
    w.Key("refresh_token"); w.String(refresh_token.c_str(), static_cast<rapidjson::SizeType>(refresh_token.size()));
    w.EndObject();

    res.ok(std::string(data.GetString(), data.GetSize()));
}

void UserService::log_login(const server::Request& req, std::int64_t user_id, database::LoginMethod method) {
    if (user_id <= 0) return;
    // The pg `ip` column is INET, but req.ip is resolved from the proxy headers
    // X-Forwarded-For / X-Real-IP (server::client_ip), which a client connecting
    // without a trusted proxy in front sets freely -- and client_ip only uses
    // "is this public" to *pick* an entry, never to validate one, so an
    // arbitrary string reaches here. INET rejects it, the INSERT throws, and the
    // catch below turns one forged header into a lost login record. Store NULL
    // unless the value parses as a bare IP literal, so the rest of the row
    // survives. Empty user_agent -> NULL likewise, rather than "".
    const database::Value ip_value =
        (is_ipv4_literal(req.ip) || is_ipv6_literal(req.ip)) ? database::Value(req.ip)
                                                            : database::Value(nullptr);
    const database::Value ua_value =
        req.user_agent.empty() ? database::Value(nullptr) : database::Value(req.user_agent);
    try {
        db_.execute(
            "INSERT INTO user_login_logs (user_id, login_method, ip, user_agent, created_at) "
            "VALUES (?, ?, ?, ?, ?);",
            {user_id, static_cast<int>(method), ip_value, ua_value,
             platform::now_unix_ms()});
    } catch (const std::exception& e) {
        platform::log_warn("audit: login log failed: %s", e.what());
    }
}

//------------------------------------------------------------------------------
// Routes
//------------------------------------------------------------------------------

// POST /email/login — start an email login: send a verification code to the
// address in the JSON body. Mirrors the Python email_login_handler.
//
// Body:    {"email": "alice@example.com"}
// Success: {"code": 0, "msg": "ok", "data": {"email": …}}
void UserService::on_email_login(const server::Request& req, server::Response& res) {
    if (!email_validator_) {
        res.error(-1, "Invalid email validator.");
        return;
    }

    rapidjson::Document doc;
    doc.Parse(req.body.c_str(), req.body.size());
    if (doc.HasParseError() || !doc.IsObject()) {
        res.error(-2, "Invalid JSON body.");
        return;
    }

    const std::string email = get_field(doc, "email");

    // send() validates the address itself (empty / missing '@' -> error),
    // short-circuits predefined addresses, and enforces the resend cooldown.
    mirobody::optional<std::string> err = email_validator_->send(email);
    if (err) {
        res.error(-3, *err);
        return;
    }

    // Echo the address back as the data object: {"email": "<email>"}.
    rapidjson::StringBuffer data;
    rapidjson::Writer<rapidjson::StringBuffer> dw(data);
    dw.StartObject();
    dw.Key("email"); dw.String(email.c_str(), static_cast<rapidjson::SizeType>(email.size()));
    dw.EndObject();

    res.ok(std::string(data.GetString(), data.GetSize()));
}

// POST /email/verify — finish an email login: check the code, create-or-get
// the user, and return auth tokens. Mirrors the Python email_verify_handler.
//
// Body:    {"email": "alice@example.com", "code": "123456"}
// Success: {"code": 0, "msg": "ok",
//           "data": {"access_token","token_type","expires_in","refresh_token"}}
void UserService::on_email_verify(const server::Request& req, server::Response& res) {
    if (!email_validator_) {
        res.error(-1, "Invalid email validator.");
        return;
    }

    rapidjson::Document doc;
    doc.Parse(req.body.c_str(), req.body.size());
    if (doc.HasParseError() || !doc.IsObject()) {
        res.error(-2, "Invalid JSON body.");
        return;
    }

    const std::string email = get_field(doc, "email");
    const std::string code  = get_field(doc, "code");

    mirobody::optional<std::string> verr = email_validator_->verify(email, code);
    if (verr) {
        res.error(-3, *verr);
        return;
    }

    std::string uerr;
    std::int64_t user_id = add_or_get_user(email, &uerr);
    if (user_id <= 0) {
        res.error(-4, uerr.empty() ? std::string("Not found.") : uerr);
        return;
    }

    log_login(req, user_id, database::LoginMethod::Email);
    generate_auth_response(res, user_id, email);
}

// POST /email/bind — start binding an email to the *currently logged-in* account:
// send a verification code to the address. Authenticated (Bearer access token).
//
// Body:    {"email": "alice@example.com"}
// Success: {"code": 0, "msg": "ok", "data": {"email": …}}
void UserService::on_email_bind(const server::Request& req, server::Response& res) {
    if (req.user_id <= 0) {
        res.error(-1, "Not authenticated.");
        return;
    }
    if (!email_validator_) {
        res.error(-2, "Invalid email validator.");
        return;
    }

    rapidjson::Document doc;
    doc.Parse(req.body.c_str(), req.body.size());
    if (doc.HasParseError() || !doc.IsObject()) {
        res.error(-3, "Invalid JSON body.");
        return;
    }

    const std::string email = get_field(doc, "email");

    mirobody::optional<std::string> err = email_validator_->send(email);
    if (err) {
        res.error(-4, *err);
        return;
    }

    rapidjson::StringBuffer data;
    rapidjson::Writer<rapidjson::StringBuffer> dw(data);

    dw.StartObject();
    dw.Key("email"); dw.String(email.c_str(), static_cast<rapidjson::SizeType>(email.size()));
    dw.EndObject();

    res.ok(std::string(data.GetString(), data.GetSize()));
}

// POST /email/bind/verify — finish binding: check the code, then set the address
// as the logged-in account's email. Authenticated. Rejects an email already held
// by another active account (-5); there is no auto-merge (see the Database
// backends README). Returns a fresh auth envelope carrying the bound email.
//
// Body:    {"email": "alice@example.com", "code": "123456"}
// Success: {"code": 0, "msg": "ok",
//           "data": {"access_token","token_type","expires_in","refresh_token"}}
void UserService::on_email_bind_verify(const server::Request& req, server::Response& res) {
    if (req.user_id <= 0) {
        res.error(-1, "Not authenticated.");
        return;
    }
    if (!email_validator_) {
        res.error(-2, "Invalid email validator.");
        return;
    }

    rapidjson::Document doc;
    doc.Parse(req.body.c_str(), req.body.size());
    if (doc.HasParseError() || !doc.IsObject()) {
        res.error(-3, "Invalid JSON body.");
        return;
    }

    const std::string email = get_field(doc, "email");
    const std::string code  = get_field(doc, "code");

    mirobody::optional<std::string> verr = email_validator_->verify(email, code);
    if (verr) {
        res.error(-4, *verr);
        return;
    }

    std::string berr;
    if (!bind_email(req.user_id, email, &berr)) {
        res.error(-5, berr.empty() ? std::string("Bind failed.") : berr);
        return;
    }

    generate_auth_response(res, req.user_id, email);
}

// POST /firebase/verify — exchange a Firebase ID token for app tokens. The
// body carries the token from any Firebase-brokered sign-in (Google, X /
// Twitter, GitHub, …). We verify the token with FirebaseTokenValidator, then
// identify the account by a *verified* email when present, else by the
// provider identity (firebase.sign_in_provider + sub) recorded in
// user_identities, and return the same auth envelope as /email/verify. (On
// the legacy schema, which has no user_identities, an absent/unverified email
// is rejected instead.)
//
// GET returns the Firebase web-app config (apiKey, authDomain, projectId,
// messagingSenderId, appId) so the web client can initialize the Firebase
// JS SDK before signing in.
//
// Body:    {"token": "<firebase id token>"}
// Success: {"code": 0, "msg": "ok",
//           "data": {"access_token","token_type","expires_in","refresh_token"}}
// Failures use sequential negative codes; the msg carries the reason.
void UserService::on_firebase_verify(const server::Request& req, server::Response& res) {
    if (req.method == "GET") {
        // code 0 + config when Google sign-in is configured; a non-zero code
        // (no data) otherwise, so the client knows to hide the button.
        if (firebase_web_config_.empty()) {
            res.error(-1, "Firebase sign-in is not configured.");
        } else {
            res.ok(firebase_web_config_);
        }
        return;
    }

    if (!firebase_) {
        res.error(-1, "Firebase sign-in is not configured.");
        return;
    }

    rapidjson::Document doc;
    doc.Parse(req.body.c_str(), req.body.size());
    if (doc.HasParseError() || !doc.IsObject()) {
        res.error(-2, "Invalid JSON body.");
        return;
    }

    const std::string token = get_field(doc, "token");
    if (token.empty()) {
        res.error(-3, "Missing token.");
        return;
    }

    jwt::FirebaseTokenValidator::VerifyResult vr = firebase_->verify_token(token);
    if (!vr.ok()) {
        res.error(-4, vr.error.empty() ? std::string("Invalid token.") : vr.error);
        return;
    }

    // Identify the user. A *verified* email is the merge key when present;
    // otherwise (no email, or unverified) fall back to the provider identity
    // (the `sub` claim) in user_identities -- never trusting an unverified
    // email as the account email. Firebase brokers the provider, so the
    // login_method comes from firebase.sign_in_provider.
    rapidjson::Document claims;
    if (claims.Parse(vr.claims_json.c_str()).HasParseError() || !claims.IsObject()) {
        res.error(-5, "Invalid token claims.");
        return;
    }
    const std::string email = get_field(claims, "email");

    std::string uerr;
    std::int64_t user_id = 0;
    if (!email.empty() && claims_email_verified(claims)) {
        user_id = add_or_get_user(email, &uerr);
    } else {
        user_id = add_or_get_identity_user(
            firebase_login_method(claims), get_field(claims, "sub"), email, &uerr);
    }
    if (user_id <= 0) {
        res.error(-6, uerr.empty() ? std::string("Not found.") : uerr);
        return;
    }

    // The provider Firebase brokered (google/github/twitter/apple/password)
    // is the login method; the same claim used to resolve the identity above.
    log_login(req, user_id, static_cast<database::LoginMethod>(firebase_login_method(claims)));
    generate_auth_response(res, user_id, email);
}

// GET/POST /apple/verify — "Sign in with Apple", the Apple sibling of
// /firebase/verify.
//
// GET returns {"clientId": ...} (code 0) when Apple sign-in is configured, so
// the web client shows an Apple button and can initialize the Apple JS SDK; a
// non-zero code (no data) otherwise so it hides the button.
//
// POST verifies the `id_token` from the Apple JS SDK against Apple's JWKS
// (AppleTokenValidator: RS256 + iss + aud + exp), then identifies the account
// by a *verified* email when present, else by the Apple subject (`sub`) via
// user_identities -- Apple omits email on repeat sign-ins. Returns the same
// auth envelope as /email/verify. (On the legacy schema an absent/unverified
// email is rejected instead.)
//
// Body:    {"id_token": "<Apple ID token>"}
// Success: {"code": 0, "msg": "ok",
//           "data": {"access_token","token_type","expires_in","refresh_token"}}
// Failures use sequential negative codes; the msg carries the reason.
void UserService::on_apple_verify(const server::Request& req, server::Response& res) {
    if (req.method == "GET") {
        if (apple_web_config_.empty()) {
            res.error(-1, "Apple sign-in is not configured.");
        } else {
            res.ok(apple_web_config_);
        }
        return;
    }

    if (!apple_) {
        res.error(-1, "Apple sign-in is not configured.");
        return;
    }

    rapidjson::Document doc;
    doc.Parse(req.body.c_str(), req.body.size());
    if (doc.HasParseError() || !doc.IsObject()) {
        res.error(-2, "Invalid JSON body.");
        return;
    }

    // Accept either `id_token` (Apple's field name) or `token` (for parity
    // with /firebase/verify), preferring the former.
    std::string token = get_field(doc, "id_token");
    if (token.empty()) { token = get_field(doc, "token"); }
    if (token.empty()) {
        res.error(-3, "Missing id_token.");
        return;
    }

    jwt::AppleTokenValidator::VerifyResult vr = apple_->verify_token(token);
    if (!vr.ok()) {
        res.error(-4, vr.error.empty() ? std::string("Invalid token.") : vr.error);
        return;
    }

    // Identify the user. A *verified* email is the merge key when present;
    // otherwise (Apple omits email on repeat sign-ins, or it is unverified)
    // fall back to the Apple subject (`sub`) in user_identities -- never
    // trusting an unverified email as the account email.
    rapidjson::Document claims;
    if (claims.Parse(vr.claims_json.c_str()).HasParseError() || !claims.IsObject()) {
        res.error(-5, "Invalid token claims.");
        return;
    }
    const std::string email = get_field(claims, "email");

    std::string uerr;
    std::int64_t user_id = 0;
    if (!email.empty() && claims_email_verified(claims)) {
        user_id = add_or_get_user(email, &uerr);
    } else {
        user_id = add_or_get_identity_user(
            static_cast<int>(database::LoginMethod::Apple),
            get_field(claims, "sub"), email, &uerr);
    }
    if (user_id <= 0) {
        res.error(-6, uerr.empty() ? std::string("Not found.") : uerr);
        return;
    }

    log_login(req, user_id, database::LoginMethod::Apple);
    generate_auth_response(res, user_id, email);
}

// GET/POST /wechat/verify — the WeChat sibling of /firebase/verify.
//
// GET returns {"appid": ...} (code 0) when web sign-in is configured, so the
// web client shows a WeChat button and can build the authorize URL; a
// non-zero code (no data) otherwise so it hides the button.
//
// POST exchanges a login `code` for app tokens. There are three code sources,
// distinguished by the optional "flow" field:
//   * Mini Program (flow unset): a wx.login() code, exchanged via
//     jscode2session with the Mini Program credentials.
//   * Web (flow "web"): an OAuth code from WeChat's qrconnect / oauth2
//     authorize, exchanged via sns/oauth2/access_token with the web
//     credentials.
//   * Mobile app (flow "app"): an OAuth code from the WeChat OpenSDK on
//     Android / iOS, exchanged via sns/oauth2/access_token with the mobile
//     app credentials.
// The web/app credentials fall back to the Mini Program ones when unset.
// Either way we resolve the user's openid/unionid, create-or-get the user by
// openid, and return the same auth envelope as /email/verify.
//
// Body:    {"code": "<login code>", "flow": "web"|"app"?}
// Success: {"code": 0, "msg": "ok",
//           "data": {"access_token","token_type","expires_in","refresh_token"}}
// Failures use sequential negative codes; the msg carries the reason.
void UserService::on_wechat_verify(const server::Request& req, server::Response& res) {
    if (req.method == "GET") {
        if (wechat_web_config_.empty()) {
            res.error(-1, "WeChat sign-in is not configured.");
        } else {
            res.ok(wechat_web_config_);
        }
        return;
    }

    rapidjson::Document doc;
    doc.Parse(req.body.c_str(), req.body.size());
    if (doc.HasParseError() || !doc.IsObject()) {
        res.error(-2, "Invalid JSON body.");
        return;
    }

    const std::string code = get_field(doc, "code");
    if (code.empty()) {
        res.error(-3, "Missing code.");
        return;
    }

    // Pick the credentials and WeChat endpoint by flow. The web and app flows
    // both use the OAuth2 endpoint (with their own credentials); the Mini
    // Program flow (default) uses jscode2session + the Mini Program creds.
    const std::string flow = get_field(doc, "flow");
    const bool oauth = (flow == "web" || flow == "app");
    std::string appid, secret;
    if (flow == "app")      { appid = wechat_app_appid_; secret = wechat_app_secret_; }
    else if (flow == "web") { appid = wechat_web_appid_; secret = wechat_web_secret_; }
    else                    { appid = wechat_appid_;     secret = wechat_secret_; }
    if (appid.empty() || secret.empty()) {
        res.error(-1, "WeChat sign-in is not configured.");
        return;
    }

    // Percent-encode the code into the query string. appid/secret are our
    // own fixed credentials, but the code is attacker-supplied input.
    std::string enc_code;
    enc_code.reserve(code.size() * 3);
    for (unsigned char c : code) {
        if (std::isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') {
            enc_code.push_back(static_cast<char>(c));
        } else {
            static const char* hex = "0123456789ABCDEF";
            enc_code.push_back('%');
            enc_code.push_back(hex[c >> 4]);
            enc_code.push_back(hex[c & 0x0F]);
        }
    }

    const std::string url = oauth
        ? (wechat_api_base_ + "/sns/oauth2/access_token?appid=" + appid +
           "&secret=" + secret + "&code=" + enc_code +
           "&grant_type=authorization_code")
        : (wechat_api_base_ + "/sns/jscode2session?appid=" + appid +
           "&secret=" + secret + "&js_code=" + enc_code +
           "&grant_type=authorization_code");

    client::HttpClient http;
    client::HttpResponse resp = http.get(url, /*timeout_ms=*/10000);
    if (resp.status != 200) {
        res.error(-4, "WeChat code exchange failed (HTTP " +
                             std::to_string(resp.status) + ").");
        return;
    }

    rapidjson::Document jr;
    jr.Parse(resp.body.c_str(), resp.body.size());
    if (jr.HasParseError() || !jr.IsObject()) {
        res.error(-5, "Invalid WeChat response.");
        return;
    }

    // Both WeChat endpoints report failure with a non-zero errcode + errmsg
    // (e.g. 40029 invalid code, 45011 rate limit) and a 200 status.
    if (jr.HasMember("errcode") && jr["errcode"].IsInt() && jr["errcode"].GetInt() != 0) {
        const std::string m = get_field(jr, "errmsg");
        res.error(-6, m.empty() ? std::string("WeChat code exchange error.") : m);
        return;
    }

    const std::string openid  = get_field(jr, "openid");
    const std::string unionid = get_field(jr, "unionid");
    if (openid.empty()) {
        res.error(-7, "WeChat response has no openid.");
        return;
    }

    std::string uerr;
    std::int64_t user_id = add_or_get_wechat_user(openid, unionid, &uerr);
    if (user_id <= 0) {
        res.error(-8, uerr.empty() ? std::string("Not found.") : uerr);
        return;
    }

    // Identify the token by the same synthetic address the row carries.
    const std::string email = (unionid.empty() ? openid : unionid) + "@wechat";
    log_login(req, user_id, database::LoginMethod::WeChat);
    generate_auth_response(res, user_id, email);
}

// GET/POST /github/verify — GitHub OAuth sign-in, the GitHub sibling of
// /wechat/verify (a server-side code exchange, not a token validator).
//
// GET returns {"clientId": ...} (code 0) when GitHub sign-in is configured,
// so the web client shows a GitHub button and can build the authorize URL; a
// non-zero code (no data) otherwise so it hides the button.
//
// POST exchanges the OAuth `code` (from GitHub's authorize redirect) for a
// GitHub access token via <oauth_base>/login/oauth/access_token, then reads
// the user's primary verified email from <api_base>/user/emails (the secret
// stays server-side throughout), creates-or-gets the user by that email, and
// returns the same auth envelope as /email/verify.
//
// Body:    {"code": "<oauth code>"}
// Success: {"code": 0, "msg": "ok",
//           "data": {"access_token","token_type","expires_in","refresh_token"}}
// Failures use sequential negative codes; the msg carries the reason.
void UserService::on_github_verify(const server::Request& req, server::Response& res) {
    if (req.method == "GET") {
        if (github_web_config_.empty()) {
            res.error(-1, "GitHub sign-in is not configured.");
        } else {
            res.ok(github_web_config_);
        }
        return;
    }

    if (github_client_id_.empty() || github_client_secret_.empty()) {
        res.error(-1, "GitHub sign-in is not configured.");
        return;
    }

    rapidjson::Document doc;
    doc.Parse(req.body.c_str(), req.body.size());
    if (doc.HasParseError() || !doc.IsObject()) {
        res.error(-2, "Invalid JSON body.");
        return;
    }

    const std::string code = get_field(doc, "code");
    if (code.empty()) {
        res.error(-3, "Missing code.");
        return;
    }

    // 1) Exchange the code for a GitHub access token. The body is
    // form-encoded; Accept: application/json makes GitHub answer with JSON
    // rather than its default form-encoded body.
    client::HttpRequest treq;
    treq.url          = github_oauth_base_ + "/login/oauth/access_token";
    treq.content_type = "application/x-www-form-urlencoded";
    treq.headers.push_back("Accept: application/json");
    treq.body = "client_id=" + url_encode(github_client_id_) +
                "&client_secret=" + url_encode(github_client_secret_) +
                "&code=" + url_encode(code);

    client::HttpClient http;
    client::HttpResponse tresp = http.post(treq);
    if (tresp.status != 200) {
        res.error(-4, "GitHub code exchange failed (HTTP " +
                             std::to_string(tresp.status) + ").");
        return;
    }

    rapidjson::Document tj;
    tj.Parse(tresp.body.c_str(), tresp.body.size());
    if (tj.HasParseError() || !tj.IsObject()) {
        res.error(-5, "Invalid GitHub token response.");
        return;
    }
    // GitHub reports a bad/expired code with an `error` field and HTTP 200.
    if (tj.HasMember("error")) {
        const std::string desc = get_field(tj, "error_description");
        res.error(-6, desc.empty() ? std::string("GitHub code exchange error.") : desc);
        return;
    }
    const std::string access = get_field(tj, "access_token");
    if (access.empty()) {
        res.error(-7, "GitHub response has no access token.");
        return;
    }

    // 2) Resolve the user's email. GitHub requires a User-Agent header on
    // every API request; user:email scope makes /user/emails list the
    // private addresses too, so we can pick the primary verified one.
    std::vector<std::string> api_headers;
    api_headers.push_back("Authorization: Bearer " + access);
    api_headers.push_back("Accept: application/vnd.github+json");
    api_headers.push_back("User-Agent: mirobody");

    client::HttpResponse eresp = http.get(github_api_base_ + "/user/emails",
                                          /*timeout_ms=*/10000, api_headers);

    std::string email;
    if (eresp.status == 200) {
        rapidjson::Document ej;
        ej.Parse(eresp.body.c_str(), eresp.body.size());
        if (!ej.HasParseError() && ej.IsArray()) {
            // Prefer the primary verified address; fall back to the first
            // verified one if none is flagged primary.
            std::string verified_fallback;
            for (rapidjson::SizeType i = 0; i < ej.Size(); ++i) {
                const rapidjson::Value& e = ej[i];
                if (!e.IsObject() || !e.HasMember("email") || !e["email"].IsString()) continue;
                const bool verified = e.HasMember("verified") && e["verified"].IsBool() && e["verified"].GetBool();
                const bool primary  = e.HasMember("primary")  && e["primary"].IsBool()  && e["primary"].GetBool();
                if (!verified) continue;
                const std::string addr(e["email"].GetString(), e["email"].GetStringLength());
                if (primary) { email = addr; break; }
                if (verified_fallback.empty()) verified_fallback = addr;
            }
            if (email.empty()) email = verified_fallback;
        }
    }

    // Fall back to the public profile email (may be null if the user keeps
    // it private and the token lacks user:email scope).
    if (email.empty()) {
        client::HttpResponse uresp = http.get(github_api_base_ + "/user",
                                             /*timeout_ms=*/10000, api_headers);
        if (uresp.status == 200) {
            rapidjson::Document uj;
            uj.Parse(uresp.body.c_str(), uresp.body.size());
            if (!uj.HasParseError() && uj.IsObject()) {
                email = get_field(uj, "email");
            }
        }
    }

    if (email.empty()) {
        res.error(-8, "GitHub account has no verified email.");
        return;
    }

    std::string uerr;
    std::int64_t user_id = add_or_get_user(email, &uerr);
    if (user_id <= 0) {
        res.error(-9, uerr.empty() ? std::string("Not found.") : uerr);
        return;
    }

    log_login(req, user_id, database::LoginMethod::GitHub);
    generate_auth_response(res, user_id, email);
}

void UserService::register_routes(server::Router& router) {
    router.post("/email/login",  [this](const server::Request& q, server::Response& s){ on_email_login(q, s); });
    router.post("/email/verify", [this](const server::Request& q, server::Response& s){ on_email_verify(q, s); });
    router.post("/email/bind", [this](const server::Request& q, server::Response& s){ on_email_bind(q, s); });
    router.post("/email/bind/verify",
                [this](const server::Request& q, server::Response& s){ on_email_bind_verify(q, s); });
    router.http("/firebase/verify", [this](const server::Request& q, server::Response& s){ on_firebase_verify(q, s); },
                server::GET | server::POST);
    router.http("/apple/verify", [this](const server::Request& q, server::Response& s){ on_apple_verify(q, s); },
                server::GET | server::POST);
    router.http("/wechat/verify", [this](const server::Request& q, server::Response& s){ on_wechat_verify(q, s); },
                server::GET | server::POST);
    router.http("/github/verify", [this](const server::Request& q, server::Response& s){ on_github_verify(q, s); },
                server::GET | server::POST);
    router.get("/auth/providers", [this](const server::Request& q, server::Response& s){ on_auth_providers(q, s); });
}

// GET /auth/providers -- the sign-in capability document. Public and
// unauthenticated by design: a client needs it to render the sign-in screen,
// before it has any credentials. Everything in it is already public (the
// per-provider GET routes serve the same values); no secret is exposed.
//
// Always code 0 -- "no provider is configured" is a valid answer, not an error.
// Callers read the per-provider `enabled` flags rather than the envelope code.
void UserService::on_auth_providers(const server::Request& req, server::Response& res) {
    (void)req;
    res.ok(auth_providers_);
}

}}

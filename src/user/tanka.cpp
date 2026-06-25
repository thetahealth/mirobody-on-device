#include "user/tanka.hpp"

#include "client/http_client.hpp"
#include "platform/log.hpp"   // log_info / log_warn / log_error

#include <rapidjson/document.h>
#include <rapidjson/stringbuffer.h>
#include <rapidjson/writer.h>

#include <cctype>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <string>
#include <vector>

namespace mirobody { namespace user {

namespace {

// Tanka's signing-WASM path (the content-hashed filename under web_origin_). NOT
// configurable: it is a MATCHED set with the vendored htdoc/static/tanka-signer.js
// (the get_http_header ABI can change across Tanka builds), so it can't be swapped
// independently. This constant and the signer are regenerated TOGETHER by
// `node res/tanka/discover.cjs --write`, which rewrites the literal below, so keep
// it on one line in this exact shape.
const char* const kTankaWasmPath = "/aa30528cabf11b4d389e.module.wasm";

// The number of string args htdoc/src/tanka.js passes to get_http_header (11 + a
// trailing bool). Auto-discovery refuses to adopt a build whose arg count differs
// (an ABI change needs tanka.js updated + an htdoc rebuild -- can't be done at
// runtime), keeping the working pinned build instead.
const int kTankaExpectedNargs = 11;

// Read a string member from `doc`. Returns "" when absent or another type.
std::string json_str(const rapidjson::Document& doc, const char* key) {
    if (!doc.IsObject() || !doc.HasMember(key)) return std::string();
    const rapidjson::Value& v = doc[key];
    return v.IsString() ? std::string(v.GetString(), v.GetStringLength()) : std::string();
}

std::string ascii_lower(const std::string& s) {
    std::string out;
    out.reserve(s.size());
    for (char c : s) out.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
    return out;
}

// Request headers the browser may pass through to Tanka. Anything else in the
// signed envelope's `headers` map is dropped, so a compromised page can't smuggle
// arbitrary headers (e.g. a different Host) through us. Lowercased keys.
bool header_allowed(const std::string& lower_key) {
    static const char* const kAllowed[] = {
        "appid", "version", "aiversion", "timezone", "deviceid", "devicetype",
        "userid", "orgid", "timestamp", "cookie", "authorization", "content-type",
        "x-t-sign", "x-t-nonce", "x-t-sign-ver", "x-t-return-sign-result",
    };
    for (const char* k : kAllowed) {
        if (lower_key == k) return true;
    }
    return false;
}

// A value that looks like an email: an '@' with a non-empty, space-free local part
// and a domain that contains a dot. Deliberately lenient (Tanka's payload shape
// isn't documented; we just need to recognize the address).
bool looks_like_email(const std::string& s) {
    std::size_t at = s.find('@');
    if (at == std::string::npos || at == 0 || at + 1 >= s.size()) return false;
    if (s.find(' ') != std::string::npos) return false;
    const std::string domain = s.substr(at + 1);
    return domain.find('.') != std::string::npos && domain.find('@') == std::string::npos;
}

// Decode a base64url string (JWT segment), padding-agnostic. Returns "" on a bad
// character. Local copy of the jwt/*.cpp helper.
std::string b64url_decode(const std::string& in) {
    std::size_t len = in.size();
    while (len > 0 && in[len - 1] == '=') --len;
    auto dec = [](char c) -> int {
        if (c >= 'A' && c <= 'Z') return c - 'A';
        if (c >= 'a' && c <= 'z') return c - 'a' + 26;
        if (c >= '0' && c <= '9') return c - '0' + 52;
        if (c == '-') return 62;
        if (c == '_') return 63;
        return -1;
    };
    std::string out;
    out.reserve((len * 3) / 4);
    std::uint32_t buf = 0;
    int bits = 0;
    for (std::size_t i = 0; i < len; ++i) {
        int v = dec(in[i]);
        if (v < 0) return std::string();
        buf = (buf << 6) | static_cast<std::uint32_t>(v);
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            out.push_back(static_cast<char>((buf >> bits) & 0xFFu));
        }
    }
    return out;
}

// Find an email anywhere in a Tanka payload (its shape isn't documented). Prefers
// a value under an email-ish key (email/emailAddress/mail); failing that, the
// first string that simply looks like an email. Recursive.
std::string dig_email(const rapidjson::Value& v, bool by_key) {
    if (v.IsObject()) {
        if (by_key) {
            for (auto it = v.MemberBegin(); it != v.MemberEnd(); ++it) {
                if (it->value.IsString()) {
                    const std::string key = ascii_lower(it->name.GetString());
                    if ((key == "email" || key == "emailaddress" || key == "mail")) {
                        const std::string val(it->value.GetString(), it->value.GetStringLength());
                        if (looks_like_email(val)) return val;
                    }
                }
            }
        }
        for (auto it = v.MemberBegin(); it != v.MemberEnd(); ++it) {
            std::string got = dig_email(it->value, by_key);
            if (!got.empty()) return got;
        }
    } else if (v.IsArray()) {
        for (rapidjson::SizeType i = 0; i < v.Size(); ++i) {
            std::string got = dig_email(v[i], by_key);
            if (!got.empty()) return got;
        }
    } else if (!by_key && v.IsString()) {
        const std::string s(v.GetString(), v.GetStringLength());
        if (looks_like_email(s)) return s;
    }
    return std::string();
}

// True for a JWT-looking string (three base64url segments). The token's field name
// in Tanka's payload isn't documented, so we locate it by shape.
bool looks_like_jwt(const std::string& s) {
    std::size_t d1 = s.find('.');
    if (d1 == std::string::npos || d1 < 8) return false;
    std::size_t d2 = s.find('.', d1 + 1);
    if (d2 == std::string::npos || d2 - d1 - 1 < 8 || s.size() - d2 - 1 < 8) return false;
    if (s.find('.', d2 + 1) != std::string::npos) return false;
    auto ok = [](char c) {
        return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
               (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.';
    };
    for (char c : s) if (!ok(c)) return false;
    return true;
}

// Collect every JWT-looking string anywhere in a payload (depth-first).
void find_jwts(const rapidjson::Value& v, std::vector<std::string>& out) {
    if (v.IsString()) {
        const std::string s(v.GetString(), v.GetStringLength());
        if (looks_like_jwt(s)) out.push_back(s);
    } else if (v.IsObject()) {
        for (auto it = v.MemberBegin(); it != v.MemberEnd(); ++it) find_jwts(it->value, out);
    } else if (v.IsArray()) {
        for (rapidjson::SizeType i = 0; i < v.Size(); ++i) find_jwts(v[i], out);
    }
}

// Email from a confirmed poll payload: a nested email-ish field if present, else
// the `email` claim of any JWT found in the payload (trusted by provenance -- the
// token came straight from Tanka over TLS). "" when none is found.
std::string resolve_email(const rapidjson::Value& data) {
    std::string email = dig_email(data, /*by_key=*/true);
    if (!email.empty()) return email;
    email = dig_email(data, /*by_key=*/false);
    if (!email.empty()) return email;

    std::vector<std::string> jwts;
    find_jwts(data, jwts);
    for (const std::string& token : jwts) {
        std::size_t d1 = token.find('.');
        std::size_t d2 = token.find('.', d1 + 1);
        const std::string payload = b64url_decode(token.substr(d1 + 1, d2 - d1 - 1));
        if (payload.empty()) continue;
        rapidjson::Document claims;
        if (claims.Parse(payload.c_str(), payload.size()).HasParseError()) continue;
        email = dig_email(claims, /*by_key=*/true);
        if (email.empty()) email = dig_email(claims, /*by_key=*/false);
        if (!email.empty()) return email;
    }
    return std::string();
}

// Run `cmd` (a full shell command line) and capture its stdout into *out. The
// child's stderr is inherited (so discover.cjs diagnostics land in the server
// log). Returns true if the process was spawned (regardless of exit code) --
// callers judge success by whether stdout parses, not by the exit status, which
// pclose reports differently across platforms. Blocking; run off the main loop.
bool run_capture(const std::string& cmd, std::string* out) {
#if defined(_WIN32)
    FILE* p = _popen(cmd.c_str(), "r");
#else
    FILE* p = popen(cmd.c_str(), "r");
#endif
    if (p == nullptr) return false;
    char buf[4096];
    std::size_t n;
    while ((n = std::fread(buf, 1, sizeof(buf), p)) > 0) out->append(buf, n);
#if defined(_WIN32)
    _pclose(p);
#else
    pclose(p);
#endif
    return true;
}

// On-disk cache path for the bridged WASM: "<tempdir>/mirobody-tanka-sign.wasm".
std::string default_wasm_cache() {
    const char* dir = std::getenv("TMPDIR");
    if (dir == nullptr || !*dir) dir = std::getenv("TEMP");
    if (dir == nullptr || !*dir) dir = std::getenv("TMP");
    std::string base = (dir != nullptr && *dir) ? std::string(dir) : std::string(".");
    if (!base.empty() && (base.back() == '/' || base.back() == '\\')) base.pop_back();
    return base + "/mirobody-tanka-sign.wasm";
}

// Parse the browser's {headers, body} envelope and forward the already-signed
// request to Tanka's gateway (server-side; the browser can't -- CORS). `body` is
// the exact JSON string the signature was computed over, sent verbatim; the
// `headers` map is filtered to the forward allowlist and the Origin Tanka expects
// is pinned. Returns Tanka's raw response body and writes the HTTP status to
// *status_out (<= 0 on a transport failure).
std::string tanka_forward(const std::string& url, const std::string& origin,
                          const std::string& env_body, int timeout_s, long* status_out) {
    *status_out = 0;
    rapidjson::Document env;
    if (env.Parse(env_body.c_str(), env_body.size()).HasParseError() || !env.IsObject()) {
        return std::string();
    }

    client::HttpRequest treq;
    treq.url = url;
    rapidjson::Value::ConstMemberIterator bit = env.FindMember("body");
    if (bit != env.MemberEnd() && bit->value.IsString()) {
        treq.body.assign(bit->value.GetString(), bit->value.GetStringLength());
    }

    bool has_ct = false;
    rapidjson::Value::ConstMemberIterator hit = env.FindMember("headers");
    if (hit != env.MemberEnd() && hit->value.IsObject()) {
        for (auto m = hit->value.MemberBegin(); m != hit->value.MemberEnd(); ++m) {
            if (!m->value.IsString()) continue;
            const std::string key(m->name.GetString(), m->name.GetStringLength());
            const std::string val(m->value.GetString(), m->value.GetStringLength());
            const std::string lk = ascii_lower(key);
            if (!header_allowed(lk)) continue;
            // Drop any value carrying CR/LF -- defends against header injection
            // even though these come from our own same-origin signer.
            if (val.find('\r') != std::string::npos || val.find('\n') != std::string::npos) continue;
            if (lk == "content-type") has_ct = true;
            treq.headers.push_back(key + ": " + val);
        }
    }
    if (!has_ct) treq.headers.push_back("Content-Type: application/json");
    treq.headers.push_back("Origin: " + origin);   // the origin Tanka pins
    treq.request_timeout_ms = timeout_s * 1000;

    client::HttpClient http;
    client::HttpResponse resp = http.request("POST", treq);
    *status_out = resp.status;
    return resp.body;
}

}  // namespace

//------------------------------------------------------------------------------

TankaService::TankaService(server::Router& router, const Config& cfg, LoginFn on_login)
    : on_login_(std::move(on_login)),
      enabled_(cfg.tanka_login_enabled),
      api_base_(cfg.tanka_api_base),
      create_path_(cfg.tanka_qrcode_create_path),
      check_path_(cfg.tanka_qrcode_check_path),
      web_origin_(cfg.tanka_web_origin),
      success_code_(cfg.tanka_success_code),
      http_timeout_(cfg.tanka_http_timeout),
      autodiscover_(cfg.tanka_autodiscover),
      refresh_interval_(cfg.tanka_refresh_interval),
      node_bin_(cfg.tanka_node_bin),
      discover_script_(cfg.tanka_discover_script),
      signer_js_path_(cfg.tanka_signer_js_path) {
    register_routes(router);
    start_autodiscovery();
}

TankaService::~TankaService() {
    {
        std::lock_guard<std::mutex> lock(refresh_mu_);
        stopping_ = true;
    }
    refresh_cv_.notify_all();
    if (refresh_thread_.joinable()) refresh_thread_.join();
}

void TankaService::register_routes(server::Router& router) {
    router.get ("/tanka/verify", [this](const server::Request& q, server::Response& s){ on_verify(q, s); });
    router.post("/tanka/qrcode", [this](const server::Request& q, server::Response& s){ on_qrcode(q, s); });
    router.post("/tanka/poll",   [this](const server::Request& q, server::Response& s){ on_poll(q, s); });
    // The signer assets the browser loads, under the app prefix so they resolve at
    // appBase()+"/tanka-sign.wasm" and appBase()+"/tanka-signer.js". The signer-js
    // route overrides the vendored static file so auto-discovery can swap the glue
    // in lockstep with the WASM.
    router.get ("/tanka-sign.wasm", [this](const server::Request& q, server::Response& s){ on_wasm(q, s); });
    router.get ("/tanka-signer.js", [this](const server::Request& q, server::Response& s){ on_signer_js(q, s); });
}

// GET /tanka/verify -- advertises whether Tanka sign-in is enabled, so the web
// client shows or hides the button. No app credentials to return (the server is a
// pure proxy), so an empty data object suffices.
void TankaService::on_verify(const server::Request& req, server::Response& res) {
    (void)req;
    if (!enabled_) {
        res.error(-1, "Tanka sign-in is not configured.");
        return;
    }
    res.ok("{}");
}

// POST /tanka/qrcode -- forward the signed create-QR request to Tanka and return
// {qrCodeData, expireTime} for the web client to render as a QR code.
void TankaService::on_qrcode(const server::Request& req, server::Response& res) {
    if (!enabled_) {
        res.error(-1, "Tanka sign-in is not configured.");
        return;
    }

    long status = 0;
    const std::string body = tanka_forward(api_base_ + create_path_, web_origin_,
                                           req.body, http_timeout_, &status);
    if (status != 200) {
        res.error(-2, "Could not reach Tanka to create the login QR.");
        return;
    }

    rapidjson::Document env;
    if (env.Parse(body.c_str(), body.size()).HasParseError() || !env.IsObject()) {
        res.error(-3, "Invalid Tanka response.");
        return;
    }
    rapidjson::Value::ConstMemberIterator ci = env.FindMember("code");
    const int code = (ci != env.MemberEnd() && ci->value.IsInt()) ? ci->value.GetInt() : -1;
    if (code != success_code_) {
        res.error(-4, "Tanka rejected the QR request (code " + std::to_string(code) + ").");
        return;
    }
    rapidjson::Value::ConstMemberIterator di = env.FindMember("data");
    if (di == env.MemberEnd() || !di->value.IsObject()) {
        res.error(-5, "Tanka response has no data.");
        return;
    }
    const rapidjson::Value& data = di->value;

    rapidjson::Value::ConstMemberIterator qi = data.FindMember("qrCodeData");
    if (qi == data.MemberEnd() || !qi->value.IsString()) {
        res.error(-6, "Tanka response has no qrCodeData.");
        return;
    }

    rapidjson::StringBuffer buf;
    rapidjson::Writer<rapidjson::StringBuffer> w(buf);
    w.StartObject();
    w.Key("qrCodeData");
    w.String(qi->value.GetString(), qi->value.GetStringLength());
    rapidjson::Value::ConstMemberIterator ei = data.FindMember("expireTime");
    if (ei != data.MemberEnd()) {
        w.Key("expireTime");
        if (ei->value.IsInt64())      w.Int64(ei->value.GetInt64());
        else if (ei->value.IsUint64()) w.Uint64(ei->value.GetUint64());
        else if (ei->value.IsNumber()) w.Double(ei->value.GetDouble());
        else if (ei->value.IsString()) w.String(ei->value.GetString(), ei->value.GetStringLength());
        else                           w.Null();
    }
    w.EndObject();
    res.ok(std::string(buf.GetString(), buf.GetSize()));
}

// POST /tanka/poll -- forward the signed poll request. While the scan is pending
// returns {status:"pending"|"expired"}; on a confirmed scan resolves the email and
// hands it to on_login_, which creates-or-gets the user and writes the auth
// envelope.
void TankaService::on_poll(const server::Request& req, server::Response& res) {
    if (!enabled_) {
        res.error(-1, "Tanka sign-in is not configured.");
        return;
    }

    long status = 0;
    const std::string body = tanka_forward(api_base_ + check_path_, web_origin_,
                                           req.body, http_timeout_, &status);
    if (status != 200) {
        res.error(-2, "Tanka login check failed.");
        return;
    }

    rapidjson::Document env;
    if (env.Parse(body.c_str(), body.size()).HasParseError() || !env.IsObject()) {
        res.error(-3, "Invalid Tanka response.");
        return;
    }
    rapidjson::Value::ConstMemberIterator ci = env.FindMember("code");
    const int code = (ci != env.MemberEnd() && ci->value.IsInt()) ? ci->value.GetInt() : -1;

    // Tanka's loginStatus (web client): 0 = waiting for scan/confirm, 2 = confirmed
    // (login payload + token present), 4 = expired/cancelled.
    int login_status = -1;
    const rapidjson::Value* data = nullptr;
    if (code == success_code_) {
        rapidjson::Value::ConstMemberIterator di = env.FindMember("data");
        if (di != env.MemberEnd() && di->value.IsObject()) {
            data = &di->value;
            rapidjson::Value::ConstMemberIterator li = data->FindMember("loginStatus");
            if (li != data->MemberEnd() && li->value.IsInt()) login_status = li->value.GetInt();
        }
    }
    const bool has_token = data != nullptr &&
        (data->HasMember("accessToken") || data->HasMember("token"));
    const bool confirmed = (login_status == 2) || has_token;
    if (!confirmed) {
        const char* st = (login_status == 4) ? "expired" : "pending";
        res.ok(std::string("{\"status\":\"") + st + "\"}");
        return;
    }

    const std::string email = resolve_email(*data);
    if (email.empty()) {
        // Confirmed but no email anywhere in the payload. The field isn't
        // documented; log and fail rather than guessing.
        platform::log_warn("tanka: confirmed login but no email resolved from payload");
        res.error(-4, "Could not read your email from Tanka.");
        return;
    }

    on_login_(req, res, email);
}

std::string TankaService::effective_wasm_url() const {
    return adopted_wasm_url_.empty() ? (web_origin_ + kTankaWasmPath) : adopted_wasm_url_;
}

const std::string* TankaService::load_wasm() {
    std::lock_guard<std::mutex> lock(wasm_mu_);
    const std::string url = effective_wasm_url();

    // Already cached for the current effective URL (an adoption changes the URL,
    // making this a miss so we refetch the adopted build).
    if (!wasm_bytes_.empty() && wasm_bytes_url_ == url) {
        return &wasm_bytes_;
    }

    const std::string cache_path = default_wasm_cache();

    // 1) disk cache (survives restarts; also a fallback if Tanka's CDN is down) --
    // but only when its sidecar ".url" records the SAME url, so adopted bytes are
    // never served under the wrong URL (and vice versa).
    {
        std::ifstream urlf((cache_path + ".url").c_str());
        std::string cached_url;
        if (urlf) std::getline(urlf, cached_url);
        if (cached_url == url) {
            std::ifstream in(cache_path.c_str(), std::ios::binary);
            if (in) {
                std::string cached((std::istreambuf_iterator<char>(in)),
                                   std::istreambuf_iterator<char>());
                if (cached.size() >= 4 && cached.compare(0, 4, "\0asm", 4) == 0) {
                    wasm_bytes_ = std::move(cached);
                    wasm_bytes_url_ = url;
                    return &wasm_bytes_;
                }
            }
        }
    }

    // 2) fetch from Tanka's CDN (server-side -- the CDN only accepts Origin
    // web_origin_, so the browser can't).
    client::HttpClient http;
    client::HttpResponse resp = http.get(url, http_timeout_ * 1000,
                                         {"Origin: " + web_origin_});
    if (resp.status != 200 || resp.body.size() < 4 || resp.body.compare(0, 4, "\0asm", 4) != 0) {
        platform::log_warn("tanka: could not fetch signer WASM from %s (status %ld)",
                           url.c_str(), resp.status);
        return nullptr;
    }
    wasm_bytes_ = resp.body;
    wasm_bytes_url_ = url;

    // 3) persist to the disk cache + record its url (best-effort).
    {
        std::ofstream out(cache_path.c_str(), std::ios::binary | std::ios::trunc);
        if (out) out.write(wasm_bytes_.data(), static_cast<std::streamsize>(wasm_bytes_.size()));
        std::ofstream urlf((cache_path + ".url").c_str(), std::ios::trunc);
        if (urlf) urlf << url;
    }
    platform::log_info("tanka: fetched + cached signer WASM (%zu bytes) from %s",
                       wasm_bytes_.size(), url.c_str());
    return &wasm_bytes_;
}

const std::string* TankaService::signer_js() {
    std::lock_guard<std::mutex> lock(wasm_mu_);
    if (!adopted_glue_.empty()) return &adopted_glue_;     // the adopted build's glue
    if (!fallback_glue_loaded_) {
        fallback_glue_loaded_ = true;   // attempt once
        std::ifstream in(signer_js_path_.c_str(), std::ios::binary);
        if (in) {
            fallback_glue_.assign((std::istreambuf_iterator<char>(in)),
                                  std::istreambuf_iterator<char>());
        } else {
            platform::log_warn("tanka: vendored signer glue not found at %s",
                               signer_js_path_.c_str());
        }
    }
    return fallback_glue_.empty() ? nullptr : &fallback_glue_;
}

// GET /tanka-sign.wasm -- bridge the effective Tanka signing WASM (auto-discovered
// build when adopted, else the pinned one), fetched from Tanka's CDN + cached.
// no-cache because auto-discovery can swap the content under this stable URL; it
// must stay in lockstep with the glue at /tanka-signer.js (also no-cache).
void TankaService::on_wasm(const server::Request& req, server::Response& res) {
    (void)req;
    if (!enabled_) {
        res.status(404);
        res.text("not found");
        return;
    }
    const std::string* bytes = load_wasm();
    if (bytes == nullptr) {
        res.status(502);
        res.text("Tanka signer unavailable");
        return;
    }
    res.content_type("application/wasm");
    res.header("Cache-Control", "no-cache");
    res.body(*bytes);
}

// GET /tanka-signer.js -- the wasm-bindgen glue the browser imports. Serves the
// auto-discovered build's glue when adopted (matching the WASM above), else the
// vendored fallback. Overrides the static file of the same name. no-cache so an
// adoption takes effect without a stale cached module.
void TankaService::on_signer_js(const server::Request& req, server::Response& res) {
    (void)req;
    if (!enabled_) {
        res.status(404);
        res.text("not found");
        return;
    }
    const std::string* js = signer_js();
    if (js == nullptr) {
        res.status(502);
        res.text("Tanka signer unavailable");
        return;
    }
    res.content_type("text/javascript");
    res.header("Cache-Control", "no-cache");
    res.body(*js);
}

// -- auto-discovery (self-healing signer adoption) -----------------------------

void TankaService::start_autodiscovery() {
    if (!enabled_ || !autodiscover_) {
        if (enabled_) {
            platform::log_info("tanka: signer auto-discovery disabled (TANKA_WASM_AUTODISCOVER=false)");
        }
        return;
    }
    refresh_thread_ = std::thread([this] { refresh_loop(); });
}

void TankaService::refresh_loop() {
    // Refresh at boot, then every refresh_interval_ seconds until shutdown.
    for (;;) {
        refresh_once();
        std::unique_lock<std::mutex> lock(refresh_mu_);
        refresh_cv_.wait_for(lock, std::chrono::seconds(refresh_interval_),
                             [this] { return stopping_; });
        if (stopping_) return;
    }
}

bool TankaService::refresh_once() {
    // Run the Node discovery engine: it finds Tanka's live signer build and
    // VERIFIES it (signs a real create-QR; Tanka must answer code:0), printing the
    // verified build as JSON on stdout. Best-effort: missing node / unreachable
    // Tanka / nothing verified -> keep the current (pinned or previously adopted)
    // build.
    const std::string cmd = node_bin_ + " \"" + discover_script_ + "\" \"" + web_origin_ + "\"";
    std::string out;
    if (!run_capture(cmd, &out)) {
        platform::log_warn("tanka: could not run signer discovery (node missing?): %s", cmd.c_str());
        return false;
    }

    rapidjson::Document doc;
    if (doc.Parse(out.c_str(), out.size()).HasParseError() || !doc.IsObject()) {
        platform::log_info("tanka: signer auto-discovery found no verified build; keeping current");
        return false;
    }
    const std::string wasm_url = json_str(doc, "wasmUrl");
    const std::string glue     = json_str(doc, "signerJs");
    int nargs = -1;
    { rapidjson::Value::ConstMemberIterator it = doc.FindMember("nargs");
      if (it != doc.MemberEnd() && it->value.IsInt()) nargs = it->value.GetInt(); }
    if (wasm_url.empty() || glue.empty()) {
        platform::log_info("tanka: signer auto-discovery returned no build; keeping current");
        return false;
    }
    // An ABI change (different string-arg count) needs htdoc/src/tanka.js updated +
    // rebuilt, which we can't do at runtime -- refuse to adopt so we never serve a
    // glue that mismatches the compiled frontend call.
    if (nargs != -1 && nargs != kTankaExpectedNargs) {
        platform::log_error(
            "tanka: discovered build %s takes %d get_http_header args (tanka.js expects %d). "
            "NOT adopting -- update tanka.js + rebuild, or run `node res/tanka/discover.cjs --write`.",
            wasm_url.c_str(), nargs, kTankaExpectedNargs);
        return false;
    }

    std::lock_guard<std::mutex> lock(wasm_mu_);
    if (wasm_url == adopted_wasm_url_) {
        return true;   // already on this build
    }
    const std::string old = adopted_wasm_url_.empty() ? (web_origin_ + kTankaWasmPath)
                                                      : adopted_wasm_url_;
    adopted_wasm_url_ = wasm_url;
    adopted_glue_     = glue;
    // Invalidate the WASM cache so the next /tanka-sign.wasm refetches the adopted
    // build (load_wasm keys the cache by URL, so this is belt-and-suspenders).
    wasm_bytes_.clear();
    wasm_bytes_url_.clear();
    platform::log_info("tanka: signer auto-discovery adopted %s (was %s)",
                       wasm_url.c_str(), old.c_str());
    return true;
}

}}

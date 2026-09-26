#include "jwt/google.hpp"
#include <optional>

#include "client/http_client.hpp"

#include <openssl/bn.h>
#include <openssl/core_names.h>
#include <openssl/evp.h>
#include <openssl/param_build.h>

#include <rapidjson/document.h>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <ctime>
#include <mutex>
#include <string>
#include <unordered_map>
#include <utility>

namespace mirobody { namespace jwt {

namespace {

//------------------------------------------------------------------------------

// Base64url decode, padding-optional. Local copy of the same helper that
// lives in jwt.cpp; if a fifth caller turns up (fernet, store, jwt, this,
// + ...) it's time to hoist into a utils/base64 module.
std::optional<std::string> b64url_decode(const std::string& in) {
    std::size_t len = in.size();
    while (len > 0 && in[len - 1] == '=') --len;

    auto decode_char = [](char c) -> int {
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
        char c = in[i];
        int v = decode_char(c);
        if (v < 0) return std::nullopt;
        buf = (buf << 6) | static_cast<std::uint32_t>(v);
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            out.push_back(static_cast<char>((buf >> bits) & 0xFFu));
        }
    }
    return out;
}

//------------------------------------------------------------------------------

// Build an EVP_PKEY (RSA public key) from a JWK's base64url-encoded
// modulus and exponent. Returns nullptr on any parse / OpenSSL failure;
// caller owns the result and must EVP_PKEY_free.
EVP_PKEY* jwk_rsa_to_pkey(const std::string& n_b64, const std::string& e_b64) {
    auto n_bytes = b64url_decode(n_b64);
    auto e_bytes = b64url_decode(e_b64);
    if (!n_bytes || !e_bytes) return nullptr;

    BIGNUM* n = BN_bin2bn(reinterpret_cast<const unsigned char*>(n_bytes->data()),
                          static_cast<int>(n_bytes->size()), nullptr);
    BIGNUM* e = BN_bin2bn(reinterpret_cast<const unsigned char*>(e_bytes->data()),
                          static_cast<int>(e_bytes->size()), nullptr);

    OSSL_PARAM_BLD* bld = OSSL_PARAM_BLD_new();
    if (!bld || !n || !e) {
        if (n)   BN_free(n);
        if (e)   BN_free(e);
        if (bld) OSSL_PARAM_BLD_free(bld);
        return nullptr;
    }

    const bool pushed = (OSSL_PARAM_BLD_push_BN(bld, OSSL_PKEY_PARAM_RSA_N, n) == 1) &&
                        (OSSL_PARAM_BLD_push_BN(bld, OSSL_PKEY_PARAM_RSA_E, e) == 1);
    OSSL_PARAM* params = pushed ? OSSL_PARAM_BLD_to_param(bld) : nullptr;
    OSSL_PARAM_BLD_free(bld);
    BN_free(n);
    BN_free(e);
    if (!params) return nullptr;

    EVP_PKEY*     pkey = nullptr;
    EVP_PKEY_CTX* ctx  = EVP_PKEY_CTX_new_from_name(nullptr, "RSA", nullptr);
    if (ctx) {
        if (EVP_PKEY_fromdata_init(ctx) > 0) {
            if (EVP_PKEY_fromdata(ctx, &pkey, EVP_PKEY_PUBLIC_KEY, params) <= 0) {
                if (pkey) { EVP_PKEY_free(pkey); pkey = nullptr; }
            }
        }
        EVP_PKEY_CTX_free(ctx);
    }
    OSSL_PARAM_free(params);
    return pkey;
}

//------------------------------------------------------------------------------

bool rsa_verify_sha256(EVP_PKEY* pkey,
                       const std::string& signing_input,
                       const std::string& signature) {
    if (!pkey) return false;
    EVP_MD_CTX* ctx = EVP_MD_CTX_new();
    if (!ctx) return false;

    bool ok = false;
    if (EVP_DigestVerifyInit(ctx, nullptr, EVP_sha256(), nullptr, pkey) == 1 &&
        EVP_DigestVerifyUpdate(ctx,
            reinterpret_cast<const unsigned char*>(signing_input.data()),
            signing_input.size()) == 1) {
        ok = EVP_DigestVerifyFinal(ctx,
                  reinterpret_cast<const unsigned char*>(signature.data()),
                  signature.size()) == 1;
    }
    EVP_MD_CTX_free(ctx);
    return ok;
}

//------------------------------------------------------------------------------

std::int64_t now_seconds() {
    return static_cast<std::int64_t>(std::time(nullptr));
}

//------------------------------------------------------------------------------

// Extract a string field from a rapidjson object, returning nullptr if
// absent or not a string. Used to keep the JWKS / claims parsing readable.
const char* json_str(const rapidjson::Value& obj, const char* name) {
    if (!obj.IsObject() || !obj.HasMember(name) || !obj[name].IsString()) return nullptr;
    return obj[name].GetString();
}

//------------------------------------------------------------------------------

// Parse `max-age=<N>` (in seconds) out of a Cache-Control header value
// and clamp into [min_secs, max_secs]. Returns `fallback` if the
// directive is absent or unparseable. Clamping defends against a
// misconfigured server pushing us into a never-refresh or
// hammer-on-every-call state.
std::chrono::seconds parse_max_age(const std::string& cache_control,
                                   std::chrono::seconds fallback,
                                   std::chrono::seconds min_secs,
                                   std::chrono::seconds max_secs) {
    if (cache_control.empty()) return fallback;

    std::string lower;
    lower.reserve(cache_control.size());
    for (auto c : cache_control) {
        lower.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
    }

    const std::string needle = "max-age=";
    auto pos = lower.find(needle);
    if (pos == std::string::npos) return fallback;
    pos += needle.size();
    while (pos < lower.size() && (lower[pos] == ' ' || lower[pos] == '\t')) ++pos;

    std::int64_t value = 0;
    bool any_digit = false;
    while (pos < lower.size() && lower[pos] >= '0' && lower[pos] <= '9') {
        value = value * 10 + (lower[pos] - '0');
        any_digit = true;
        ++pos;
        if (value > max_secs.count()) { value = max_secs.count(); break; }
    }
    if (!any_digit) return fallback;
    if (value < min_secs.count()) value = min_secs.count();
    return std::chrono::seconds(value);
}

}

//------------------------------------------------------------------------------
// Impl: JWKS cache + key lookup
//------------------------------------------------------------------------------

struct GoogleTokenValidator::Impl {
    using clock = std::chrono::steady_clock;

    // Fallback TTL when the response carries no usable Cache-Control.
    // Google does send one in practice (currently max-age=21600), but
    // belt-and-suspenders: 1 hour is conservative if it goes missing.
    static constexpr std::chrono::seconds kDefaultTtl{3600};
    // Clamp range so a misconfigured server can't push us into hammering
    // (min) or never refreshing (max). Google rotates daily; cap at 24h.
    static constexpr std::chrono::seconds kMinTtl{60};
    static constexpr std::chrono::seconds kMaxTtl{86400};

    std::string                                client_id;
    std::mutex                                 mu;
    std::unordered_map<std::string, EVP_PKEY*> keys_by_kid;     // owns
    clock::time_point                          keys_expire_at = clock::time_point::min();

    explicit Impl(std::string cid) : client_id(std::move(cid)) {}

    ~Impl() {
        for (auto& kv : keys_by_kid) {
            if (kv.second) EVP_PKEY_free(kv.second);
        }
    }

    // Fetch the JWKS from Google and rebuild the cache. Caller holds mu.
    // Returns true on success.
    bool refresh_locked() {
        client::HttpClient http;
        auto resp = http.get(kGoogleJwkUrl, /*timeout_ms=*/10000);
        if (resp.status < 200 || resp.status >= 300) return false;

        rapidjson::Document doc;
        if (doc.Parse(resp.body.c_str()).HasParseError() || !doc.IsObject()) return false;
        if (!doc.HasMember("keys") || !doc["keys"].IsArray()) return false;

        std::unordered_map<std::string, EVP_PKEY*> fresh;
        for (const auto& jwk : doc["keys"].GetArray()) {
            if (!jwk.IsObject()) continue;

            const char* kty = json_str(jwk, "kty");
            const char* kid = json_str(jwk, "kid");
            const char* n   = json_str(jwk, "n");
            const char* e   = json_str(jwk, "e");
            if (!kty || !kid || !n || !e || std::string(kty) != "RSA") continue;

            EVP_PKEY* pkey = jwk_rsa_to_pkey(n, e);
            if (!pkey) continue;

            auto it = fresh.find(kid);
            if (it != fresh.end()) {
                EVP_PKEY_free(it->second);
                it->second = pkey;
            } else {
                fresh.emplace(kid, pkey);
            }
        }

        if (fresh.empty()) return false;

        for (auto& kv : keys_by_kid) {
            if (kv.second) EVP_PKEY_free(kv.second);
        }
        keys_by_kid = std::move(fresh);

        // Honor the response's Cache-Control max-age, falling back to
        // the default TTL when it's absent or unparseable. Clamped into
        // [kMinTtl, kMaxTtl].
        auto cc_it = resp.headers.find("cache-control");
        const std::string cc = cc_it == resp.headers.end() ? std::string{} : cc_it->second;
        const auto ttl = parse_max_age(cc, kDefaultTtl, kMinTtl, kMaxTtl);
        keys_expire_at = clock::now() + ttl;
        return true;
    }

    // Look up the public key for `kid`. Refreshes the cache lazily when
    // it's expired, and once more if the kid still isn't known (Google
    // sometimes ships a new signing key before the cache TTL elapses).
    // Returns nullptr if no matching key can be found.
    EVP_PKEY* get_key(const std::string& kid) {
        std::lock_guard<std::mutex> lk(mu);

        if (clock::now() >= keys_expire_at || keys_by_kid.empty()) {
            refresh_locked();   // best-effort; fall through to whatever we have
        }

        auto it = keys_by_kid.find(kid);
        if (it != keys_by_kid.end()) return it->second;

        if (refresh_locked()) {
            it = keys_by_kid.find(kid);
            if (it != keys_by_kid.end()) return it->second;
        }
        return nullptr;
    }
};

// Out-of-line definitions are retained for toolchains that still emit references
// when a class-type constant is passed by value.
constexpr std::chrono::seconds GoogleTokenValidator::Impl::kDefaultTtl;
constexpr std::chrono::seconds GoogleTokenValidator::Impl::kMinTtl;
constexpr std::chrono::seconds GoogleTokenValidator::Impl::kMaxTtl;

//------------------------------------------------------------------------------
// GoogleTokenValidator
//------------------------------------------------------------------------------

GoogleTokenValidator::GoogleTokenValidator(std::string client_id)
    : impl_(new Impl(std::move(client_id))) {}

//------------------------------------------------------------------------------

GoogleTokenValidator::~GoogleTokenValidator() = default;
GoogleTokenValidator::GoogleTokenValidator(GoogleTokenValidator&&) noexcept = default;
GoogleTokenValidator& GoogleTokenValidator::operator=(GoogleTokenValidator&&) noexcept = default;

//------------------------------------------------------------------------------

GoogleTokenValidator::VerifyResult
GoogleTokenValidator::verify_token(const std::string& id_token) {
    VerifyResult r;

    if (id_token.empty()) { r.error = "Empty Google token"; return r; }

    // Three segments separated by exactly two dots.
    const auto first_dot = id_token.find('.');
    if (first_dot == std::string::npos)  { r.error = "Malformed Google token"; return r; }
    const auto second_dot = id_token.find('.', first_dot + 1);
    if (second_dot == std::string::npos) { r.error = "Malformed Google token"; return r; }
    if (id_token.find('.', second_dot + 1) != std::string::npos) {
        r.error = "Malformed Google token"; return r;
    }

    const auto header_b64  = id_token.substr(0, first_dot);
    const auto payload_b64 = id_token.substr(first_dot + 1, second_dot - first_dot - 1);
    const auto sig_b64     = id_token.substr(second_dot + 1);

    // Header carries `alg` (must be RS256) and `kid` (selects the key).
    auto header_bytes = b64url_decode(header_b64);
    if (!header_bytes) { r.error = "Invalid Google token header encoding"; return r; }

    rapidjson::Document header_doc;
    if (header_doc.Parse(header_bytes->c_str()).HasParseError() || !header_doc.IsObject()) {
        r.error = "Invalid Google token header JSON";
        return r;
    }
    const char* alg = json_str(header_doc, "alg");
    if (!alg || std::string(alg) != "RS256") {
        r.error = "Google token alg is not RS256";
        return r;
    }
    const char* kid = json_str(header_doc, "kid");
    if (!kid) {
        r.error = "Google token missing kid";
        return r;
    }

    EVP_PKEY* pkey = impl_->get_key(kid);
    if (!pkey) {
        r.error = std::string("Google JWKS lookup failed for kid: ") + kid;
        return r;
    }

    auto sig_bytes = b64url_decode(sig_b64);
    if (!sig_bytes) { r.error = "Invalid Google token signature encoding"; return r; }

    const std::string signing_input = std::string(header_b64) + "." + std::string(payload_b64);
    if (!rsa_verify_sha256(pkey, signing_input, *sig_bytes)) {
        r.error = "Google token verification failed: bad signature";
        return r;
    }

    auto payload_bytes = b64url_decode(payload_b64);
    if (!payload_bytes) { r.error = "Invalid Google token payload encoding"; return r; }

    rapidjson::Document payload_doc;
    if (payload_doc.Parse(payload_bytes->c_str()).HasParseError() || !payload_doc.IsObject()) {
        r.error = "Invalid Google token payload JSON";
        return r;
    }

    // exp (required).
    std::int64_t exp = 0;
    if (payload_doc.HasMember("exp")) {
        const auto& e = payload_doc["exp"];
        if      (e.IsInt64())  exp = e.GetInt64();
        else if (e.IsInt())    exp = e.GetInt();
        else if (e.IsUint())   exp = static_cast<std::int64_t>(e.GetUint());
        else if (e.IsUint64()) exp = static_cast<std::int64_t>(e.GetUint64());
    }
    if (!exp)                  { r.error = "Google token missing exp claim"; return r; }
    if (now_seconds() >= exp)  { r.error = "Google token expired.";          return r; }

    // iss must be Google. Google has historically issued both forms.
    const char* iss = json_str(payload_doc, "iss");
    if (!iss) { r.error = "Google token missing iss claim"; return r; }
    const std::string iss_s(iss);
    if (iss_s != kGoogleIssuer && iss_s != "accounts.google.com") {
        r.error = "Google token iss not from Google: " + iss_s;
        return r;
    }

    // aud must equal our client_id.
    const char* aud = json_str(payload_doc, "aud");
    if (!aud) { r.error = "Google token missing aud claim"; return r; }
    if (impl_->client_id != aud) {
        r.error = "Google token invalid audience, expected: " + impl_->client_id;
        return r;
    }

    // email must be present and non-empty.
    const char* email = json_str(payload_doc, "email");
    if (!email || *email == '\0') {
        r.error = "Google token missing email claim.";
        return r;
    }

    r.claims_json = std::move(*payload_bytes);
    return r;
}

}
}

#include "jwt/firebase.hpp"

#include "client/http_client.hpp"

#include <algorithm>
#include <vector>

#include <openssl/bio.h>
#include <openssl/evp.h>
#include <openssl/pem.h>
#include <openssl/x509.h>

#include <rapidjson/document.h>

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

// Base64url decode, padding-optional. Local copy of the helper that lives
// in jwt.cpp / google.cpp; the comment over there owns the "hoist once we
// have a fifth caller" reminder.
mirobody::optional<std::string> b64url_decode(const std::string& in) {
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
        if (v < 0) return mirobody::nullopt;
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

// Parse a PEM-encoded X.509 certificate and extract its public key. The
// Firebase JWKS endpoint serves certs (not raw JWKs), one per kid, so
// this is the entry point for the cache. Returns nullptr on any OpenSSL
// failure; caller owns the result and must EVP_PKEY_free.
EVP_PKEY* pem_x509_to_pubkey(const std::string& pem) {
    BIO* bio = BIO_new_mem_buf(pem.data(), static_cast<int>(pem.size()));
    if (!bio) return nullptr;

    X509* cert = PEM_read_bio_X509(bio, nullptr, nullptr, nullptr);
    BIO_free(bio);
    if (!cert) return nullptr;

    EVP_PKEY* pkey = X509_get_pubkey(cert);   // bumps refcount, caller owns
    X509_free(cert);
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

const char* json_str(const rapidjson::Value& obj, const char* name) {
    if (!obj.IsObject() || !obj.HasMember(name) || !obj[name].IsString()) return nullptr;
    return obj[name].GetString();
}

}

//------------------------------------------------------------------------------
// Impl: cert-bundle cache + key lookup
//------------------------------------------------------------------------------

struct FirebaseTokenValidator::Impl {
    using clock = std::chrono::steady_clock;

    // Fixed cache TTL. The Python sibling honors the response's
    // `Cache-Control: max-age=`; HttpResponse does not surface headers
    // today, so we use the Python default (1 hour). Google rotates the
    // securetoken certs on a similar cadence, so this is conservative.
    static constexpr std::chrono::seconds kCacheTtl{3600};

    // Every accepted project, and the issuer each one implies. Parallel vectors
    // rather than a map: there are one or two entries in practice, so a linear
    // scan beats a hash, and keeping them ordered makes the error messages list
    // the expectations in configuration order.
    std::vector<std::string>                   project_ids;
    std::vector<std::string>                   issuers;      // derived from project_ids
    std::mutex                                 mu;
    std::unordered_map<std::string, EVP_PKEY*> keys_by_kid;  // owns
    clock::time_point                          keys_expire_at = clock::time_point::min();

    explicit Impl(std::vector<std::string> pids) {
        for (std::size_t i = 0; i < pids.size(); ++i) {
            if (pids[i].empty()) continue;   // an unset config slot, not a project
            issuers.push_back("https://securetoken.google.com/" + pids[i]);
            project_ids.push_back(std::move(pids[i]));
        }
    }

    // Joined for an error message: "a, b" -- so a mismatch says what WAS allowed
    // rather than just that the token failed.
    static std::string join(const std::vector<std::string>& v) {
        std::string out;
        for (std::size_t i = 0; i < v.size(); ++i) {
            if (i) out += ", ";
            out += v[i];
        }
        return out;
    }

    ~Impl() {
        for (auto& kv : keys_by_kid) {
            if (kv.second) EVP_PKEY_free(kv.second);
        }
    }

    // Fetch the cert bundle and rebuild the cache. Caller holds mu.
    // Returns true on success.
    bool refresh_locked() {
        client::HttpClient http;
        auto resp = http.get(kFirebaseJwkUrl, /*timeout_ms=*/10000);
        if (resp.status < 200 || resp.status >= 300) return false;

        rapidjson::Document doc;
        if (doc.Parse(resp.body.c_str()).HasParseError() || !doc.IsObject()) return false;

        std::unordered_map<std::string, EVP_PKEY*> fresh;
        for (auto it = doc.MemberBegin(); it != doc.MemberEnd(); ++it) {
            if (!it->name.IsString() || !it->value.IsString()) continue;

            const char* kid = it->name.GetString();
            std::string pem(it->value.GetString(), it->value.GetStringLength());

            EVP_PKEY* pkey = pem_x509_to_pubkey(pem);
            if (!pkey) continue;

            auto fit = fresh.find(kid);
            if (fit != fresh.end()) {
                EVP_PKEY_free(fit->second);
                fit->second = pkey;
            } else {
                fresh.emplace(kid, pkey);
            }
        }

        if (fresh.empty()) return false;

        for (auto& kv : keys_by_kid) {
            if (kv.second) EVP_PKEY_free(kv.second);
        }
        keys_by_kid    = std::move(fresh);
        keys_expire_at = clock::now() + kCacheTtl;
        return true;
    }

    EVP_PKEY* get_key(const std::string& kid) {
        std::lock_guard<std::mutex> lk(mu);

        if (clock::now() >= keys_expire_at || keys_by_kid.empty()) {
            refresh_locked();   // best-effort
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

// Out-of-line definition for the static constexpr member. Required because
// kCacheTtl is odr-used (bound by const reference in `clock::now() + kCacheTtl`
// above) and this translation unit is compiled as C++11 — under C++17 the
// member would be implicitly inline and this definition unnecessary.
constexpr std::chrono::seconds FirebaseTokenValidator::Impl::kCacheTtl;

//------------------------------------------------------------------------------
// FirebaseTokenValidator
//------------------------------------------------------------------------------

FirebaseTokenValidator::FirebaseTokenValidator(std::string project_id)
    : impl_(new Impl(std::vector<std::string>(1, std::move(project_id)))) {}

FirebaseTokenValidator::FirebaseTokenValidator(std::vector<std::string> project_ids)
    : impl_(new Impl(std::move(project_ids))) {}

//------------------------------------------------------------------------------

FirebaseTokenValidator::~FirebaseTokenValidator() = default;
FirebaseTokenValidator::FirebaseTokenValidator(FirebaseTokenValidator&&) noexcept = default;
FirebaseTokenValidator& FirebaseTokenValidator::operator=(FirebaseTokenValidator&&) noexcept = default;

//------------------------------------------------------------------------------

FirebaseTokenValidator::VerifyResult
FirebaseTokenValidator::verify_token(const std::string& id_token) {
    VerifyResult r;

    if (id_token.empty()) { r.error = "Empty Firebase token"; return r; }

    const auto first_dot = id_token.find('.');
    if (first_dot == std::string::npos)  { r.error = "Malformed Firebase token"; return r; }
    const auto second_dot = id_token.find('.', first_dot + 1);
    if (second_dot == std::string::npos) { r.error = "Malformed Firebase token"; return r; }
    if (id_token.find('.', second_dot + 1) != std::string::npos) {
        r.error = "Malformed Firebase token"; return r;
    }

    const auto header_b64  = id_token.substr(0, first_dot);
    const auto payload_b64 = id_token.substr(first_dot + 1, second_dot - first_dot - 1);
    const auto sig_b64     = id_token.substr(second_dot + 1);

    auto header_bytes = b64url_decode(header_b64);
    if (!header_bytes) { r.error = "Invalid Firebase token header encoding"; return r; }

    rapidjson::Document header_doc;
    if (header_doc.Parse(header_bytes->c_str()).HasParseError() || !header_doc.IsObject()) {
        r.error = "Invalid Firebase token header JSON";
        return r;
    }
    const char* alg = json_str(header_doc, "alg");
    if (!alg || std::string(alg) != "RS256") {
        r.error = "Firebase token alg is not RS256";
        return r;
    }
    const char* kid = json_str(header_doc, "kid");
    if (!kid) {
        r.error = "No kid in token header.";
        return r;
    }

    EVP_PKEY* pkey = impl_->get_key(kid);
    if (!pkey) {
        r.error = std::string("Could not get public key for kid ") + kid;
        return r;
    }

    auto sig_bytes = b64url_decode(sig_b64);
    if (!sig_bytes) { r.error = "Invalid Firebase token signature encoding"; return r; }

    const std::string signing_input = std::string(header_b64) + "." + std::string(payload_b64);
    if (!rsa_verify_sha256(pkey, signing_input, *sig_bytes)) {
        r.error = "Firebase token verification failed: bad signature";
        return r;
    }

    auto payload_bytes = b64url_decode(payload_b64);
    if (!payload_bytes) { r.error = "Invalid Firebase token payload encoding"; return r; }

    rapidjson::Document payload_doc;
    if (payload_doc.Parse(payload_bytes->c_str()).HasParseError() || !payload_doc.IsObject()) {
        r.error = "Invalid Firebase token payload JSON";
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
    if (!exp)                  { r.error = "Firebase token missing exp claim"; return r; }
    if (now_seconds() >= exp)  { r.error = "Firebase token expired.";          return r; }

    // iss must be `https://securetoken.google.com/<project_id>` for one of the
    // accepted projects.
    const char* iss = json_str(payload_doc, "iss");
    if (!iss) { r.error = "Firebase token missing iss claim"; return r; }
    if (std::find(impl_->issuers.begin(), impl_->issuers.end(), iss) == impl_->issuers.end()) {
        r.error = "Firebase token iss mismatch, expected one of: " + Impl::join(impl_->issuers);
        return r;
    }

    // aud must equal one of the accepted project ids. Checked separately from iss
    // rather than trusting that a matching iss implies it: they are independent
    // claims, and a token pairing project A's issuer with project B's audience
    // must not pass on the strength of either half.
    const char* aud = json_str(payload_doc, "aud");
    if (!aud) { r.error = "Firebase token missing aud claim"; return r; }
    if (std::find(impl_->project_ids.begin(), impl_->project_ids.end(), aud) == impl_->project_ids.end()) {
        r.error = "Firebase token invalid audience, expected one of: " + Impl::join(impl_->project_ids);
        return r;
    }

    // email is optional: a provider may withhold it (e.g. X / Twitter), and we
    // only trust a *verified* email anyway. When absent or unverified the caller
    // identifies the account by the provider subject (`sub`) instead. The full
    // payload (email, email_verified, sub, firebase.sign_in_provider) is returned
    // for the caller to read.
    r.claims_json = std::move(*payload_bytes);
    return r;
}

}
}

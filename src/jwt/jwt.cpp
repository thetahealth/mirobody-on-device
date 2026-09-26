#include "jwt/jwt.hpp"
#include <optional>

#include "jwt/encoder.hpp"

#include <openssl/bio.h>
#include <openssl/bn.h>
#include <openssl/buffer.h>
#include <openssl/core_names.h>
#include <openssl/crypto.h>
#include <openssl/evp.h>
#include <openssl/hmac.h>
#include <openssl/pem.h>
#include <openssl/rand.h>
#include <openssl/sha.h>

#include <rapidjson/document.h>
#include <rapidjson/stringbuffer.h>
#include <rapidjson/writer.h>

#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace mirobody { namespace jwt {

namespace {

//------------------------------------------------------------------------------

// Base64url encode without trailing `=` padding, as required by JWT.
std::string b64url_encode(const std::uint8_t* data, std::size_t len) {
    static constexpr char alphabet[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZ"
        "abcdefghijklmnopqrstuvwxyz"
        "0123456789-_";
    std::string out;
    out.reserve(((len * 4) + 2) / 3);
    for (std::size_t i = 0; i < len; i += 3) {
        std::uint32_t n = static_cast<std::uint32_t>(data[i]) << 16;
        std::size_t take = 1;
        if (i + 1 < len) { n |= static_cast<std::uint32_t>(data[i + 1]) << 8; take = 2; }
        if (i + 2 < len) { n |= static_cast<std::uint32_t>(data[i + 2]);      take = 3; }
        out.push_back(alphabet[(n >> 18) & 0x3F]);
        out.push_back(alphabet[(n >> 12) & 0x3F]);
        if (take >= 2) out.push_back(alphabet[(n >> 6) & 0x3F]);
        if (take >= 3) out.push_back(alphabet[n & 0x3F]);
    }
    return out;
}

std::string b64url_encode(const std::string& s) {
    return b64url_encode(reinterpret_cast<const std::uint8_t*>(s.data()), s.size());
}

//------------------------------------------------------------------------------

// Base64url decode. Accepts inputs with or without `=` padding.
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

std::string hmac_sha256(const std::string& key, const std::string& msg) {
    unsigned char out[EVP_MAX_MD_SIZE];
    unsigned int out_len = 0;
    HMAC(EVP_sha256(),
         key.data(), static_cast<int>(key.size()),
         reinterpret_cast<const unsigned char*>(msg.data()), msg.size(),
         out, &out_len);
    return std::string(reinterpret_cast<const char*>(out), out_len);
}

//------------------------------------------------------------------------------

// Constant-time byte comparison. Length mismatch short-circuits (length
// itself is not secret) but identical-length compares run to completion.
bool ct_equal(const std::string& a, const std::string& b) {
    if (a.size() != b.size()) return false;
    if (a.empty()) return true;
    return CRYPTO_memcmp(a.data(), b.data(), a.size()) == 0;
}

//------------------------------------------------------------------------------

std::string strip_bearer(const std::string& s) {
    const std::string prefix = "Bearer ";
    std::size_t i = 0;
    while (s.size() - i >= prefix.size() && s.compare(i, prefix.size(), prefix) == 0) {
        i += prefix.size();
    }
    return s.substr(i);
}

//------------------------------------------------------------------------------

std::int64_t now_seconds() {
    return static_cast<std::int64_t>(std::time(nullptr));
}

//------------------------------------------------------------------------------

// RAII for EVP_MD_CTX so signing / verifying paths do not have to thread
// manual `EVP_MD_CTX_free` through every error branch.
struct MdCtxGuard {
    EVP_MD_CTX* ctx;
    MdCtxGuard() : ctx(EVP_MD_CTX_new()) {}
    ~MdCtxGuard() { if (ctx) EVP_MD_CTX_free(ctx); }
    MdCtxGuard(const MdCtxGuard&)            = delete;
    MdCtxGuard& operator=(const MdCtxGuard&) = delete;
};

//------------------------------------------------------------------------------

EVP_PKEY* parse_private_pem(const std::string& pem) {
    if (pem.empty()) return nullptr;
    BIO* bio = BIO_new_mem_buf(pem.data(), static_cast<int>(pem.size()));
    if (!bio) return nullptr;
    EVP_PKEY* key = PEM_read_bio_PrivateKey(bio, nullptr, nullptr, nullptr);
    BIO_free(bio);
    return key;
}

EVP_PKEY* parse_public_pem(const std::string& pem) {
    if (pem.empty()) return nullptr;
    BIO* bio = BIO_new_mem_buf(pem.data(), static_cast<int>(pem.size()));
    if (!bio) return nullptr;
    EVP_PKEY* key = PEM_read_bio_PUBKEY(bio, nullptr, nullptr, nullptr);
    BIO_free(bio);
    return key;
}

//------------------------------------------------------------------------------

// A positive BIGNUM as the minimal big-endian byte string, base64url-encoded —
// the form JWK uses for RSA `n` and `e` (RFC 7518 sec.6.3.1).
std::string bn_b64url(const BIGNUM* bn) {
    const int len = BN_num_bytes(bn);
    if (len <= 0) return std::string();
    std::string raw(static_cast<std::size_t>(len), '\0');
    BN_bn2bin(bn, reinterpret_cast<unsigned char*>(&raw[0]));
    return b64url_encode(reinterpret_cast<const std::uint8_t*>(raw.data()), raw.size());
}

// Build one JWK object string and the RFC 7638 thumbprint `kid` for the RSA
// public half of `pkey`. Best-effort: returns false (outputs untouched) when the
// key is not RSA or its params can't be read. The thumbprint hashes the
// canonical JWK `{"e":..,"kty":"RSA","n":..}` (members lexicographically ordered,
// no whitespace).
bool rsa_jwk_object(EVP_PKEY* pkey, std::string* jwk_out, std::string* kid_out) {
    if (!pkey) return false;
    BIGNUM* n = nullptr;
    BIGNUM* e = nullptr;
    if (EVP_PKEY_get_bn_param(pkey, OSSL_PKEY_PARAM_RSA_N, &n) != 1 ||
        EVP_PKEY_get_bn_param(pkey, OSSL_PKEY_PARAM_RSA_E, &e) != 1) {
        if (n) BN_free(n);
        if (e) BN_free(e);
        return false;
    }
    const std::string n_b64 = bn_b64url(n);
    const std::string e_b64 = bn_b64url(e);
    BN_free(n);
    BN_free(e);
    if (n_b64.empty() || e_b64.empty()) return false;

    const std::string thumb_input =
        "{\"e\":\"" + e_b64 + "\",\"kty\":\"RSA\",\"n\":\"" + n_b64 + "\"}";
    unsigned char digest[SHA256_DIGEST_LENGTH];
    SHA256(reinterpret_cast<const unsigned char*>(thumb_input.data()),
           thumb_input.size(), digest);
    const std::string kid = b64url_encode(digest, sizeof(digest));

    *kid_out = kid;
    *jwk_out =
        "{\"kty\":\"RSA\",\"use\":\"sig\",\"alg\":\"RS256\",\"kid\":\"" +
        kid + "\",\"n\":\"" + n_b64 + "\",\"e\":\"" + e_b64 + "\"}";
    return true;
}

}

//------------------------------------------------------------------------------
// Jwt (abstract base)
//------------------------------------------------------------------------------

Jwt::Jwt(std::string iss, std::string aud, std::string client_id,
         std::string scope, std::int64_t expires_in, std::string salt)
    : iss_(std::move(iss)),
      aud_(std::move(aud)),
      client_id_(std::move(client_id)),
      scope_(std::move(scope)),
      expires_in_(expires_in > 0 ? expires_in : 60 * 60 * 24 * 30),
      salt_(std::move(salt)) {}

//------------------------------------------------------------------------------

// Salt the subject encoding with the token's `exp` so the same row id maps to a
// different opaque subject each issuance (issuance stamps exp; verification
// reads it back to decode). Both sides must compose it identically.
static std::string subject_salt(const std::string& salt, std::int64_t exp) {
    return salt + ":" + std::to_string(exp);
}

//------------------------------------------------------------------------------

std::string Jwt::generate(std::int64_t user_id,
                          const std::unordered_map<std::string, std::string>& extra,
                          std::int64_t expires_in) const {
    rapidjson::Document doc(rapidjson::kObjectType);
    auto& alloc = doc.GetAllocator();

    const std::int64_t now       = now_seconds() - 60;
    const std::int64_t exp_after = (expires_in > 60) ? expires_in : expires_in_;

    auto add_str = [&](const char* k, const std::string& v) {
        doc.AddMember(
            rapidjson::Value(k, alloc),
            rapidjson::Value(v.data(), static_cast<rapidjson::SizeType>(v.size()), alloc),
            alloc);
    };
    auto add_int = [&](const char* k, std::int64_t v) {
        doc.AddMember(rapidjson::Value(k, alloc), rapidjson::Value(v), alloc);
    };

    // The subject is the opaque encoding of the row id, keyed by the salt + this
    // token's exp (`now + exp_after`). Jwt::verify decodes it back into
    // VerifyResult.user_id using the same exp claim.
    const std::string sub = encrypt::encode_int64(user_id, subject_salt(salt_, now + exp_after));
    // Per RFC 7519 every registered claim is optional; the notes below describe
    // what *this* system actually relies on (see do_verify and require_auth).
    add_str("sub",       sub);              // required: decoded back to the user row id; identity is lost without it
    add_str("iss",       iss_);             // optional: emitted but not checked by do_verify
    // add_str("aud",       aud_);             // optional: emitted but not checked by do_verify
    // add_int("iat",       now);              // optional: informational only
    // add_int("nbf",       now);              // optional: NOT enforced by do_verify (a future nbf is ignored)
    add_int("exp",       now + exp_after);  // required: enforced by do_verify AND keys the sub salt (a missing exp never expires and breaks decode)
    // add_str("client_id", client_id_);       // optional: OAuth metadata, not validated
    add_str("scope",     scope_);           // optional: ignored by verifier, may be read downstream for authz
    // jti (a random token id) omitted: no replay/revocation tracking reads it today

    for (const auto& kv : extra) {
        auto it = doc.FindMember(kv.first.c_str());
        if (it != doc.MemberEnd()) {
            it->value.SetString(kv.second.c_str(),
                                static_cast<rapidjson::SizeType>(kv.second.size()),
                                alloc);
        } else {
            doc.AddMember(
                rapidjson::Value(kv.first.c_str(),
                                 static_cast<rapidjson::SizeType>(kv.first.size()),
                                 alloc),
                rapidjson::Value(kv.second.c_str(),
                                 static_cast<rapidjson::SizeType>(kv.second.size()),
                                 alloc),
                alloc);
        }
    }

    rapidjson::StringBuffer buf;
    rapidjson::Writer<rapidjson::StringBuffer> writer(buf);
    doc.Accept(writer);

    // Header bytes are derived from algorithm() so the signature is over
    // the exact same prefix the verifier will reconstruct.
    const std::string alg = algorithm();
    const std::string kid = key_id();
    std::string header_json;
    header_json.reserve(20 + alg.size() + (kid.empty() ? 0 : kid.size() + 9));
    header_json.append("{\"alg\":\"");
    header_json.append(alg.data(), alg.size());
    header_json.append("\",\"typ\":\"JWT\"");
    if (!kid.empty()) {
        header_json.append(",\"kid\":\"");
        header_json.append(kid.data(), kid.size());
        header_json.append("\"");
    }
    header_json.append("}");

    const std::string header_b64  = b64url_encode(header_json);
    const std::string payload_b64 = b64url_encode(
        std::string(buf.GetString(), buf.GetSize()));
    const std::string signing_input = header_b64 + "." + payload_b64;
    const std::string sig_b64       = b64url_encode(sign(signing_input));

    return signing_input + "." + sig_b64;
}

//------------------------------------------------------------------------------

Jwt::VerifyResult Jwt::verify(const std::string& token) const {
    return do_verify(token, /*verify_exp=*/true, /*max_age=*/0);
}

//------------------------------------------------------------------------------

Jwt::VerifyResult Jwt::verify_allow_expired(const std::string& token,
                                            std::int64_t max_age) const {
    return do_verify(token, /*verify_exp=*/false, max_age);
}

//------------------------------------------------------------------------------

Jwt::VerifyResult Jwt::do_verify(const std::string& token_in,
                                  bool verify_exp,
                                  std::int64_t allow_expired_max_age) const {
    VerifyResult r;

    std::string token = strip_bearer(token_in);
    if (token.empty()) { r.error = "Empty JWT token"; return r; }

    const auto first_dot = token.find('.');
    if (first_dot == std::string::npos)  { r.error = "Malformed JWT"; return r; }
    const auto second_dot = token.find('.', first_dot + 1);
    if (second_dot == std::string::npos) { r.error = "Malformed JWT"; return r; }
    if (token.find('.', second_dot + 1) != std::string::npos) {
        r.error = "Malformed JWT"; return r;
    }

    const auto header_b64  = token.substr(0, first_dot);
    const auto payload_b64 = token.substr(first_dot + 1, second_dot - first_dot - 1);
    const auto sig_b64     = token.substr(second_dot + 1);

    const std::string signing_input = std::string(header_b64) + "." + std::string(payload_b64);

    auto sig = b64url_decode(sig_b64);
    if (!sig) { r.error = "Invalid signature encoding"; return r; }

    if (!verify_signature(signing_input, *sig)) {
        r.error = "Bad signature";
        return r;
    }

    auto payload = b64url_decode(payload_b64);
    if (!payload) { r.error = "Invalid payload encoding"; return r; }

    rapidjson::Document doc;
    if (doc.Parse(payload->c_str()).HasParseError() || !doc.IsObject()) {
        r.error = "Invalid payload JSON";
        return r;
    }

    std::int64_t exp = 0;
    if (doc.HasMember("exp")) {
        const auto& e = doc["exp"];
        if      (e.IsInt64())  exp = e.GetInt64();
        else if (e.IsInt())    exp = e.GetInt();
        else if (e.IsUint())   exp = static_cast<std::int64_t>(e.GetUint());
        else if (e.IsUint64()) exp = static_cast<std::int64_t>(e.GetUint64());
    }

    const std::int64_t now = now_seconds();
    if (verify_exp) {
        if (exp && now >= exp) { r.error = "Token expired"; return r; }
    } else if (allow_expired_max_age > 0 && exp) {
        if (now - exp > allow_expired_max_age) {
            r.error = "Token expired beyond max re-auth age";
            return r;
        }
    }

    // Decode the subject back to the row id, keyed by the salt + the token's
    // own exp (the same inputs generate() used). Left <=0 when absent / not
    // decodable; callers treat a non-positive id as unauthenticated.
    if (doc.HasMember("sub") && doc["sub"].IsString()) {
        const std::string sub(doc["sub"].GetString(), doc["sub"].GetStringLength());
        r.user_id = encrypt::decode_int64(sub, subject_salt(salt_, exp));
    }

    r.claims_json = std::move(*payload);
    return r;
}

//------------------------------------------------------------------------------
// JwtHs256
//------------------------------------------------------------------------------

JwtHs256::JwtHs256(Options opts)
    : Jwt(std::move(opts.iss), std::move(opts.aud),
          std::move(opts.client_id), std::move(opts.scope),
          opts.expires_in, std::move(opts.salt)),
      key_(std::move(opts.key)) {
    if (key_.empty()) {
        throw std::runtime_error("jwt::JwtHs256: empty key");
    }
}

//------------------------------------------------------------------------------

std::string JwtHs256::algorithm() const { return "HS256"; }

//------------------------------------------------------------------------------

std::string JwtHs256::sign(const std::string& signing_input) const {
    return hmac_sha256(key_, signing_input);
}

//------------------------------------------------------------------------------

bool JwtHs256::verify_signature(const std::string& signing_input,
                                 const std::string& signature) const {
    return ct_equal(hmac_sha256(key_, signing_input), signature);
}

//------------------------------------------------------------------------------
// JwtRs256
//------------------------------------------------------------------------------

struct JwtRs256::Impl {
    EVP_PKEY* signing_key = nullptr;          // owns; from private PEM
    std::vector<EVP_PKEY*> verify_keys;       // owns; public PEM + rotation keys
    std::string kid;                          // signer's thumbprint -> header kid
    std::string jwks_json;                    // {"keys":[...]} over all verify keys

    Impl() = default;
    ~Impl() {
        if (signing_key) EVP_PKEY_free(signing_key);
        for (EVP_PKEY* k : verify_keys) if (k) EVP_PKEY_free(k);
    }
    Impl(const Impl&)            = delete;
    Impl& operator=(const Impl&) = delete;
};

//------------------------------------------------------------------------------

JwtRs256::JwtRs256(Options opts)
    : Jwt(std::move(opts.iss), std::move(opts.aud),
          std::move(opts.client_id), std::move(opts.scope),
          opts.expires_in, std::move(opts.salt)),
      impl_(new Impl()) {
    if (!opts.private_key_pem.empty()) {
        impl_->signing_key = parse_private_pem(opts.private_key_pem);
        if (!impl_->signing_key) {
            throw std::runtime_error("jwt::JwtRs256: invalid private key PEM");
        }
    }
    // Verify set: the primary public key first (it corresponds to the signing
    // key and supplies the header `kid`), then any rotation public keys.
    if (!opts.public_key_pem.empty()) {
        EVP_PKEY* vk = parse_public_pem(opts.public_key_pem);
        if (!vk) throw std::runtime_error("jwt::JwtRs256: invalid public key PEM");
        impl_->verify_keys.push_back(vk);
    }
    for (const std::string& pem : opts.additional_public_key_pems) {
        if (pem.empty()) continue;
        EVP_PKEY* vk = parse_public_pem(pem);
        if (!vk) throw std::runtime_error("jwt::JwtRs256: invalid additional public key PEM");
        impl_->verify_keys.push_back(vk);
    }

    // The header `kid` is the thumbprint of the SIGNING key's public half — the
    // primary verify key when present, else the private key (which also carries
    // the public modulus/exponent for a sign-only setup).
    {
        EVP_PKEY* signer_pub = !impl_->verify_keys.empty()
            ? impl_->verify_keys.front() : impl_->signing_key;
        std::string jwk;
        rsa_jwk_object(signer_pub, &jwk, &impl_->kid);   // best-effort; leaves kid empty on failure
    }

    // JWKS publishes every verify key (so rotated-out keys still validate). With
    // no verify key (sign-only), fall back to the signing key's public half so a
    // deployment that configured only a private key still advertises a JWKS.
    {
        std::vector<EVP_PKEY*> jwk_keys = impl_->verify_keys;
        if (jwk_keys.empty() && impl_->signing_key) jwk_keys.push_back(impl_->signing_key);
        std::string keys;
        for (EVP_PKEY* k : jwk_keys) {
            std::string jwk, kid;
            if (!rsa_jwk_object(k, &jwk, &kid)) continue;
            if (!keys.empty()) keys += ",";
            keys += jwk;
        }
        if (!keys.empty()) impl_->jwks_json = "{\"keys\":[" + keys + "]}";
    }
}

//------------------------------------------------------------------------------

JwtRs256::~JwtRs256() = default;
JwtRs256::JwtRs256(JwtRs256&&) noexcept = default;
JwtRs256& JwtRs256::operator=(JwtRs256&&) noexcept = default;

//------------------------------------------------------------------------------

std::string JwtRs256::algorithm() const { return "RS256"; }

//------------------------------------------------------------------------------

std::string JwtRs256::key_id() const { return impl_->kid; }

const std::string& JwtRs256::kid() const { return impl_->kid; }

const std::string& JwtRs256::jwks_json() const { return impl_->jwks_json; }

//------------------------------------------------------------------------------

std::string JwtRs256::sign(const std::string& signing_input) const {
    if (!impl_->signing_key) {
        throw std::runtime_error("jwt::JwtRs256: no private key configured");
    }

    MdCtxGuard g;
    if (!g.ctx) throw std::runtime_error("jwt::JwtRs256: EVP_MD_CTX_new failed");

    if (EVP_DigestSignInit(g.ctx, nullptr, EVP_sha256(), nullptr, impl_->signing_key) != 1) {
        throw std::runtime_error("jwt::JwtRs256: EVP_DigestSignInit failed");
    }
    if (EVP_DigestSignUpdate(g.ctx,
                              reinterpret_cast<const unsigned char*>(signing_input.data()),
                              signing_input.size()) != 1) {
        throw std::runtime_error("jwt::JwtRs256: EVP_DigestSignUpdate failed");
    }

    std::size_t out_len = 0;
    if (EVP_DigestSignFinal(g.ctx, nullptr, &out_len) != 1) {
        throw std::runtime_error("jwt::JwtRs256: EVP_DigestSignFinal (size query) failed");
    }

    std::string sig(out_len, '\0');
    if (EVP_DigestSignFinal(g.ctx,
                             reinterpret_cast<unsigned char*>(&sig[0]),
                             &out_len) != 1) {
        throw std::runtime_error("jwt::JwtRs256: EVP_DigestSignFinal failed");
    }
    sig.resize(out_len);
    return sig;
}

//------------------------------------------------------------------------------

bool JwtRs256::verify_signature(const std::string& signing_input,
                                 const std::string& signature) const {
    // Accept a signature made by ANY configured verify key. The set is small (the
    // active key plus keys being rotated out), so trying each is cheap and avoids
    // depending on the header `kid` being present/correct — third-party
    // validators still use the published `kid`, but our own check is robust
    // without it.
    for (EVP_PKEY* vk : impl_->verify_keys) {
        if (!vk) continue;
        MdCtxGuard g;
        if (!g.ctx) return false;
        if (EVP_DigestVerifyInit(g.ctx, nullptr, EVP_sha256(), nullptr, vk) != 1) continue;
        if (EVP_DigestVerifyUpdate(g.ctx,
                                    reinterpret_cast<const unsigned char*>(signing_input.data()),
                                    signing_input.size()) != 1) continue;
        if (EVP_DigestVerifyFinal(g.ctx,
                                   reinterpret_cast<const unsigned char*>(signature.data()),
                                   signature.size()) == 1) {
            return true;
        }
    }
    return false;
}

//------------------------------------------------------------------------------

std::string rsa_public_pem_from_private(const std::string& private_pem) {
    EVP_PKEY* key = parse_private_pem(private_pem);
    if (!key) return std::string();

    std::string out;
    BIO* bio = BIO_new(BIO_s_mem());
    if (bio && PEM_write_bio_PUBKEY(bio, key) == 1) {
        BUF_MEM* mem = nullptr;
        BIO_get_mem_ptr(bio, &mem);
        if (mem && mem->data) out.assign(mem->data, mem->length);
    }
    if (bio) BIO_free(bio);
    EVP_PKEY_free(key);
    return out;
}

}
}

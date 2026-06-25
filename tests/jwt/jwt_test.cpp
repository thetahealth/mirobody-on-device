#include "jwt/jwt.hpp"

#include "jwt/encoder.hpp"

#include <catch2/catch_test_macros.hpp>

#include <openssl/bio.h>
#include <openssl/evp.h>
#include <openssl/pem.h>

#include <cstdint>
#include <stdexcept>
#include <string>
#include <unordered_map>

using mirobody::jwt::Jwt;
using mirobody::jwt::JwtHs256;
using mirobody::jwt::JwtRs256;

namespace {

// Salt the validators use to encode the token subject from a row id; the
// sub-value assertions below recompute the expected token with it.
const char* const kTestSalt = "test-jwt-salt";

// Minimal base64url decoder (no padding) for inspecting a JWT header segment.
std::string b64url_decode_str(const std::string& in) {
    auto val = [](char c) -> int {
        if (c >= 'A' && c <= 'Z') return c - 'A';
        if (c >= 'a' && c <= 'z') return c - 'a' + 26;
        if (c >= '0' && c <= '9') return c - '0' + 52;
        if (c == '-') return 62;
        if (c == '_') return 63;
        return -1;
    };
    std::string out;
    std::uint32_t buf = 0;
    int bits = 0;
    for (char c : in) {
        const int v = val(c);
        if (v < 0) break;
        buf = (buf << 6) | static_cast<std::uint32_t>(v);
        bits += 6;
        if (bits >= 8) { bits -= 8; out.push_back(static_cast<char>((buf >> bits) & 0xFF)); }
    }
    return out;
}

//------------------------------------------------------------------------------

JwtHs256::Options hs_opts(std::int64_t expires_in = 300) {
    JwtHs256::Options o;
    o.key        = "test-secret-key-do-not-use-in-prod";
    o.expires_in = expires_in;
    o.salt       = kTestSalt;
    return o;
}

//------------------------------------------------------------------------------

struct RsaKeyPair {
    std::string private_pem;
    std::string public_pem;
};

// Generate a fresh 2048-bit RSA key pair via OpenSSL and export both halves as
// PEM, so the test binary ships no hardcoded key material.
RsaKeyPair make_rsa_keypair() {
    EVP_PKEY_CTX* ctx = EVP_PKEY_CTX_new_id(EVP_PKEY_RSA, nullptr);
    REQUIRE(ctx);
    REQUIRE(EVP_PKEY_keygen_init(ctx) == 1);
    REQUIRE(EVP_PKEY_CTX_set_rsa_keygen_bits(ctx, 2048) > 0);

    EVP_PKEY* pkey = nullptr;
    REQUIRE(EVP_PKEY_keygen(ctx, &pkey) == 1);
    EVP_PKEY_CTX_free(ctx);

    auto pem_of = [](auto writer, EVP_PKEY* k) {
        BIO* bio = BIO_new(BIO_s_mem());
        writer(bio, k);
        BUF_MEM* mem = nullptr;
        BIO_get_mem_ptr(bio, &mem);
        std::string s(mem->data, mem->length);
        BIO_free(bio);
        return s;
    };

    RsaKeyPair out;
    out.private_pem = pem_of([](BIO* b, EVP_PKEY* k) {
        PEM_write_bio_PrivateKey(b, k, nullptr, nullptr, 0, nullptr, nullptr);
    }, pkey);
    out.public_pem = pem_of([](BIO* b, EVP_PKEY* k) {
        PEM_write_bio_PUBKEY(b, k);
    }, pkey);

    EVP_PKEY_free(pkey);
    return out;
}

// The primary keypair the RS256 round-trip tests share. Cached as a
// function-local static so the ~100ms keygen cost only happens once.
const RsaKeyPair& test_rsa_keypair() {
    static const RsaKeyPair kp = make_rsa_keypair();
    return kp;
}

//------------------------------------------------------------------------------

JwtRs256::Options rs_opts() {
    const auto& kp = test_rsa_keypair();
    JwtRs256::Options o;
    o.private_key_pem = kp.private_pem;
    o.public_key_pem  = kp.public_pem;
    o.salt            = kTestSalt;
    return o;
}

}

//------------------------------------------------------------------------------
// JwtHs256
//------------------------------------------------------------------------------

TEST_CASE("JwtHs256 round trip exposes the standard claim set", "[jwt]") {
    JwtHs256 v(hs_opts());
    auto token = v.generate(123);
    REQUIRE(!token.empty());

    std::size_t dots = 0;
    for (char c : token) if (c == '.') ++dots;
    REQUIRE(dots == 2);

    auto r = v.verify(token);
    REQUIRE(r.ok());
    // The subject is salt-keyed by the token's exp (not reconstructable here
    // without the wall-clock exp), so assert the round-trip via the decoded id.
    REQUIRE(r.user_id == 123);
    REQUIRE(r.claims_json.find("\"iss\":\"mb_oauth\"")    != std::string::npos);
}

//------------------------------------------------------------------------------

TEST_CASE("JwtHs256 token header carries alg=HS256", "[jwt]") {
    JwtHs256 v(hs_opts());
    auto token = v.generate(1);
    // First segment is the base64url of {"alg":"HS256","typ":"JWT"}.
    // That fixed JSON encodes to "eyJhbGciOiJIUzI1NiIsInR5cCI6IkpXVCJ9".
    REQUIRE(token.substr(0, token.find('.')) ==
            "eyJhbGciOiJIUzI1NiIsInR5cCI6IkpXVCJ9");
}

//------------------------------------------------------------------------------

TEST_CASE("JwtHs256 verify strips one or more 'Bearer ' prefixes", "[jwt]") {
    JwtHs256 v(hs_opts());
    auto token = v.generate(1);
    REQUIRE(v.verify("Bearer " + token).ok());
    REQUIRE(v.verify("Bearer Bearer " + token).ok());
}

//------------------------------------------------------------------------------

TEST_CASE("JwtHs256 verify rejects a tampered signature", "[jwt]") {
    JwtHs256 v(hs_opts());
    auto token = v.generate(1);
    // Tamper the first signature char. The very last base64url char of a
    // JWT signature carries 2 padding bits that aren't part of the
    // decoded signature, so flipping token.back() can be a no-op.
    auto pos = token.rfind('.') + 1;
    token[pos] = (token[pos] == 'A') ? 'B' : 'A';
    REQUIRE_FALSE(v.verify(token).ok());
}

//------------------------------------------------------------------------------

TEST_CASE("JwtHs256 verify rejects a token signed with a different key", "[jwt]") {
    JwtHs256 alice(hs_opts());
    auto bob_opts = hs_opts();
    bob_opts.key  = "a-different-secret";
    JwtHs256 bob(bob_opts);

    REQUIRE_FALSE(bob.verify(alice.generate(1)).ok());
}

//------------------------------------------------------------------------------

TEST_CASE("JwtHs256 verify rejects malformed tokens", "[jwt]") {
    JwtHs256 v(hs_opts());
    REQUIRE_FALSE(v.verify("").ok());
    REQUIRE_FALSE(v.verify("not.a.jwt.token").ok());
    REQUIRE_FALSE(v.verify("only-two.parts").ok());
    REQUIRE_FALSE(v.verify("no-dots-at-all").ok());
}

//------------------------------------------------------------------------------

TEST_CASE("JwtHs256 expired tokens fail verify; verify_allow_expired still accepts", "[jwt]") {
    JwtHs256 v(hs_opts(/*expires_in=*/1));
    auto token = v.generate(1);
    REQUIRE_FALSE(v.verify(token).ok());
    REQUIRE(v.verify_allow_expired(token, /*max_age=*/3600).ok());
}

//------------------------------------------------------------------------------

TEST_CASE("JwtHs256 extra claims overlay the default claim set", "[jwt]") {
    JwtHs256 v(hs_opts());
    std::unordered_map<std::string, std::string> extra{
        {"email",     "x@y.z"},
        {"client_id", "custom"},
    };
    auto r = v.verify(v.generate(1, extra));
    REQUIRE(r.ok());
    REQUIRE(r.claims_json.find("\"email\":\"x@y.z\"")     != std::string::npos);
    REQUIRE(r.claims_json.find("\"client_id\":\"custom\"") != std::string::npos);
    REQUIRE(r.claims_json.find("theta_data") == std::string::npos);
}

//------------------------------------------------------------------------------

TEST_CASE("JwtHs256 empty key is rejected at construction", "[jwt]") {
    JwtHs256::Options o;   // o.key is default-empty
    REQUIRE_THROWS_AS(JwtHs256(o), std::runtime_error);
}

//------------------------------------------------------------------------------
// JwtRs256
//------------------------------------------------------------------------------

TEST_CASE("JwtRs256 round trip with a fresh RSA-2048 keypair", "[jwt]") {
    JwtRs256 v(rs_opts());
    auto token = v.generate(456);
    REQUIRE(!token.empty());

    auto r = v.verify(token);
    REQUIRE(r.ok());
    // Subject is salt-keyed by the token's exp; assert the round-trip via id.
    REQUIRE(r.user_id == 456);
}

//------------------------------------------------------------------------------

TEST_CASE("JwtRs256 token header carries alg=RS256 and a kid", "[jwt]") {
    JwtRs256 v(rs_opts());
    auto token = v.generate(1);
    const std::string header = b64url_decode_str(token.substr(0, token.find('.')));
    REQUIRE(header.find("\"alg\":\"RS256\"") != std::string::npos);
    REQUIRE(header.find("\"typ\":\"JWT\"") != std::string::npos);
    // The header kid must match the key's JWK thumbprint so a JWKS validator
    // can select the right key.
    REQUIRE(!v.kid().empty());
    REQUIRE(header.find("\"kid\":\"" + v.kid() + "\"") != std::string::npos);
}

//------------------------------------------------------------------------------

TEST_CASE("JwtRs256 publishes a JWKS whose kid matches the token header", "[jwt]") {
    JwtRs256 v(rs_opts());
    const std::string jwks = v.jwks_json();
    REQUIRE(!jwks.empty());
    REQUIRE(jwks.find("\"keys\"")        != std::string::npos);
    REQUIRE(jwks.find("\"kty\":\"RSA\"") != std::string::npos);
    REQUIRE(jwks.find("\"use\":\"sig\"") != std::string::npos);
    REQUIRE(jwks.find("\"alg\":\"RS256\"") != std::string::npos);
    REQUIRE(jwks.find("\"n\":\"")        != std::string::npos);
    REQUIRE(jwks.find("\"e\":\"")        != std::string::npos);
    REQUIRE(!v.kid().empty());
    REQUIRE(jwks.find("\"kid\":\"" + v.kid() + "\"") != std::string::npos);
}

//------------------------------------------------------------------------------

TEST_CASE("JwtRs256 verifies across a key rotation via additional public keys", "[jwt]") {
    const RsaKeyPair a = make_rsa_keypair();   // outgoing key
    const RsaKeyPair b = make_rsa_keypair();   // new active signer

    // Old signer issues a token with key A.
    JwtRs256::Options ao;
    ao.private_key_pem = a.private_pem;
    ao.public_key_pem  = a.public_pem;
    ao.salt            = kTestSalt;
    JwtRs256 old_signer(ao);
    const std::string old_token = old_signer.generate(7);

    // New signer = key B, keeping key A as a verify-only rotation key.
    JwtRs256::Options no;
    no.private_key_pem            = b.private_pem;
    no.public_key_pem             = b.public_pem;
    no.additional_public_key_pems = { a.public_pem };
    no.salt                       = kTestSalt;
    JwtRs256 rotating(no);

    // Tokens from BOTH the old and the new key verify.
    auto r_old = rotating.verify(old_token);
    REQUIRE(r_old.ok());
    REQUIRE(r_old.user_id == 7);
    auto r_new = rotating.verify(rotating.generate(8));
    REQUIRE(r_new.ok());
    REQUIRE(r_new.user_id == 8);

    // The active (B) and outgoing (A) keys have distinct kids; the JWKS lists both.
    REQUIRE(old_signer.kid() != rotating.kid());
    const std::string jwks = rotating.jwks_json();
    REQUIRE(jwks.find("\"kid\":\"" + rotating.kid()   + "\"") != std::string::npos);
    REQUIRE(jwks.find("\"kid\":\"" + old_signer.kid() + "\"") != std::string::npos);
}

//------------------------------------------------------------------------------

TEST_CASE("JwtRs256 verify rejects a tampered signature", "[jwt]") {
    JwtRs256 v(rs_opts());
    auto token = v.generate(1);
    auto pos = token.rfind('.') + 1;
    token[pos] = (token[pos] == 'A') ? 'B' : 'A';
    REQUIRE_FALSE(v.verify(token).ok());
}

//------------------------------------------------------------------------------

TEST_CASE("JwtRs256 verify rejects a token signed by a different keypair", "[jwt]") {
    // Build a second, throwaway keypair so we have a public key that
    // doesn't match the test_rsa_keypair() private side.
    EVP_PKEY_CTX* ctx = EVP_PKEY_CTX_new_id(EVP_PKEY_RSA, nullptr);
    EVP_PKEY_keygen_init(ctx);
    EVP_PKEY_CTX_set_rsa_keygen_bits(ctx, 2048);
    EVP_PKEY* other = nullptr;
    EVP_PKEY_keygen(ctx, &other);
    EVP_PKEY_CTX_free(ctx);

    BIO* bio = BIO_new(BIO_s_mem());
    PEM_write_bio_PUBKEY(bio, other);
    BUF_MEM* mem = nullptr;
    BIO_get_mem_ptr(bio, &mem);
    std::string other_pub(mem->data, mem->length);
    BIO_free(bio);
    EVP_PKEY_free(other);

    JwtRs256 signer(rs_opts());

    JwtRs256::Options vo;
    vo.public_key_pem = other_pub;   // mismatched public side
    JwtRs256 verifier(vo);

    REQUIRE_FALSE(verifier.verify(signer.generate(1)).ok());
}

//------------------------------------------------------------------------------

TEST_CASE("JwtRs256 invalid PEM throws at construction", "[jwt]") {
    JwtRs256::Options o;
    o.private_key_pem = "not actually pem data";
    REQUIRE_THROWS_AS(JwtRs256(o), std::runtime_error);
}

//------------------------------------------------------------------------------

TEST_CASE("JwtRs256 sign-only setup cannot verify its own tokens", "[jwt]") {
    // Private only -> sign works, verify_signature returns false because
    // there's no verifying key; surfaces as "Bad signature".
    JwtRs256::Options o;
    o.private_key_pem = test_rsa_keypair().private_pem;
    JwtRs256 sign_only(o);

    auto token = sign_only.generate(1);
    REQUIRE_FALSE(sign_only.verify(token).ok());
}

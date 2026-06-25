#include "oauth/pkce.hpp"

#include <catch2/catch_test_macros.hpp>

#include <set>
#include <string>

using mirobody::oauth::base64url_decode;
using mirobody::oauth::base64url_encode;
using mirobody::oauth::random_token;
using mirobody::oauth::sha256;
using mirobody::oauth::verify_pkce_s256;

//------------------------------------------------------------------------------
// base64url
//------------------------------------------------------------------------------

TEST_CASE("base64url_encode is unpadded and url-safe", "[oauth][pkce]") {
    // ">>>?" -> bytes 0x3e 0x3e 0x3e 0x3f, which in standard base64 use '+'/'/'.
    // The url-safe alphabet must emit '-'/'_' instead and never '='.
    const std::string in = "\x3e\x3e\x3e\x3f";
    const std::string out = base64url_encode(in);
    REQUIRE(out.find('+') == std::string::npos);
    REQUIRE(out.find('/') == std::string::npos);
    REQUIRE(out.find('=') == std::string::npos);
    REQUIRE(out == "Pj4-Pw");
}

TEST_CASE("base64url round-trips arbitrary bytes of every length mod 3", "[oauth][pkce]") {
    for (std::size_t n = 0; n < 30; ++n) {
        std::string raw;
        for (std::size_t i = 0; i < n; ++i) raw.push_back(static_cast<char>((i * 37 + 11) & 0xFF));
        std::string decoded;
        REQUIRE(base64url_decode(base64url_encode(raw), &decoded));
        REQUIRE(decoded == raw);
    }
}

TEST_CASE("base64url_decode rejects invalid input", "[oauth][pkce]") {
    std::string out;
    REQUIRE_FALSE(base64url_decode("not valid!", &out));   // space + '!'
    REQUIRE_FALSE(base64url_decode("A", &out));            // lone 6-bit group
    // Standard-base64 symbols are not valid in the url-safe alphabet.
    REQUIRE_FALSE(base64url_decode("ab+/", &out));
}

//------------------------------------------------------------------------------
// PKCE S256 (RFC 7636)
//------------------------------------------------------------------------------

TEST_CASE("verify_pkce_s256 accepts the RFC 7636 Appendix B vector", "[oauth][pkce]") {
    // From RFC 7636 Appendix B.
    const std::string verifier  = "dBjftJeZ4CVP-mB92K27uhbUJU1p1r_wW1gFWFOEjXk";
    const std::string challenge = "E9Melhoa2OwvFrEMTJguCHaoeK1t8URWbuGJSstw-cM";
    REQUIRE(base64url_encode(sha256(verifier)) == challenge);
    REQUIRE(verify_pkce_s256(verifier, challenge));
}

TEST_CASE("verify_pkce_s256 rejects a wrong verifier and empty inputs", "[oauth][pkce]") {
    const std::string challenge = "E9Melhoa2OwvFrEMTJguCHaoeK1t8URWbuGJSstw-cM";
    REQUIRE_FALSE(verify_pkce_s256("the-wrong-verifier-value-1234567890", challenge));
    REQUIRE_FALSE(verify_pkce_s256("", challenge));
    REQUIRE_FALSE(verify_pkce_s256("dBjftJeZ4CVP-mB92K27uhbUJU1p1r_wW1gFWFOEjXk", ""));
    REQUIRE_FALSE(verify_pkce_s256("", ""));
}

//------------------------------------------------------------------------------
// random_token
//------------------------------------------------------------------------------

TEST_CASE("random_token is url-safe, sized, and non-repeating", "[oauth][pkce]") {
    std::set<std::string> seen;
    for (int i = 0; i < 64; ++i) {
        const std::string t = random_token(24);
        // 24 bytes -> 32 base64url chars (no padding).
        REQUIRE(t.size() == 32);
        REQUIRE(t.find('=') == std::string::npos);
        REQUIRE(t.find('+') == std::string::npos);
        REQUIRE(t.find('/') == std::string::npos);
        REQUIRE(seen.insert(t).second);   // no collisions across draws
    }
}

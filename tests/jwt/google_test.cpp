#include "jwt/google.hpp"

#include <catch2/catch_test_macros.hpp>

#include <string>

using mirobody::jwt::GoogleTokenValidator;

// All tests here exercise paths that fail before the JWKS HTTP fetch is
// reached, so they run offline. End-to-end verification needs either a
// real Google ID token (with the test's client_id set to its `aud`) or
// a mocked JWKS endpoint; we intentionally don't build that here.

//------------------------------------------------------------------------------

TEST_CASE("GoogleTokenValidator rejects an empty token", "[jwt][google]") {
    GoogleTokenValidator v("test-client-id.apps.googleusercontent.com");
    auto r = v.verify_token("");
    REQUIRE_FALSE(r.ok());
    REQUIRE(r.error.find("Empty") != std::string::npos);
}

//------------------------------------------------------------------------------

TEST_CASE("GoogleTokenValidator rejects malformed tokens", "[jwt][google]") {
    GoogleTokenValidator v("test-client-id.apps.googleusercontent.com");
    REQUIRE_FALSE(v.verify_token("no-dots-here").ok());
    REQUIRE_FALSE(v.verify_token("only.two.parts.too.many").ok());
    REQUIRE_FALSE(v.verify_token("two.parts").ok());
}

//------------------------------------------------------------------------------

TEST_CASE("GoogleTokenValidator rejects a non-base64 header segment", "[jwt][google]") {
    GoogleTokenValidator v("test-client-id.apps.googleusercontent.com");
    auto r = v.verify_token("@@not-base64@@.eyJ9.sig");
    REQUIRE_FALSE(r.ok());
    REQUIRE(r.error.find("header") != std::string::npos);
}

//------------------------------------------------------------------------------

TEST_CASE("GoogleTokenValidator rejects tokens whose alg is not RS256", "[jwt][google]") {
    // {"alg":"HS256","typ":"JWT"} -> "eyJhbGciOiJIUzI1NiIsInR5cCI6IkpXVCJ9"
    const std::string header_b64 = "eyJhbGciOiJIUzI1NiIsInR5cCI6IkpXVCJ9";
    const std::string body = header_b64 + ".eyJ9.AAAA";

    GoogleTokenValidator v("test-client-id.apps.googleusercontent.com");
    auto r = v.verify_token(body);
    REQUIRE_FALSE(r.ok());
    REQUIRE(r.error.find("RS256") != std::string::npos);
}

//------------------------------------------------------------------------------

TEST_CASE("GoogleTokenValidator rejects tokens that omit kid", "[jwt][google]") {
    // {"alg":"RS256","typ":"JWT"} (no kid) -> "eyJhbGciOiJSUzI1NiIsInR5cCI6IkpXVCJ9"
    const std::string header_b64 = "eyJhbGciOiJSUzI1NiIsInR5cCI6IkpXVCJ9";
    const std::string body = header_b64 + ".eyJ9.AAAA";

    GoogleTokenValidator v("test-client-id.apps.googleusercontent.com");
    auto r = v.verify_token(body);
    REQUIRE_FALSE(r.ok());
    REQUIRE(r.error.find("kid") != std::string::npos);
}

#pragma once


#include <memory>
#include <string>

namespace mirobody { namespace jwt {

// Verifier for "Sign in with Apple" ID tokens (the RS256 JWT Apple issues after
// a successful sign-in). The Apple sibling of GoogleTokenValidator -- same
// JWKS + RS256 machinery, different endpoint / issuer / audience:
//   - Pulls Apple's JWKS from https://appleid.apple.com/auth/keys on first use;
//     caches the parsed keys for the response's Cache-Control max-age (clamped
//     to [60s, 24h]; 1 hour fallback when the header is missing).
//   - Picks the right key per token by the JWT header's `kid` claim, refreshing
//     the cache once if the kid isn't known (Apple rotates its signing keys).
//   - Verifies the RS256 signature, the `iss` (must be
//     `https://appleid.apple.com`), the `aud` (must match the configured
//     client_id -- the Apple *Services ID* used by the web sign-in), the `exp`
//     (must be in the future), and asserts the payload carries an `email` claim.
//
// Apple returns `email` in the ID token whenever the `email` scope was granted
// (including Hide-My-Email relay addresses); the user's name is delivered out of
// band on first consent and is not needed here -- we identify the user by email,
// like the Google / Firebase flow.
//
// Thread-safe: the cache is protected by an internal mutex.
class AppleTokenValidator {
public:
    static constexpr const char* kAppleJwkUrl = "https://appleid.apple.com/auth/keys";
    static constexpr const char* kAppleIssuer = "https://appleid.apple.com";

    struct VerifyResult {
        std::string claims_json;   // payload JSON when ok() is true
        std::string error;
        bool ok() const { return error.empty(); }
    };

    // `client_id` is the Apple Services ID that issued the token: it must match
    // the token's `aud` claim or verification fails.
    explicit AppleTokenValidator(std::string client_id);
    ~AppleTokenValidator();

    AppleTokenValidator(const AppleTokenValidator&)            = delete;
    AppleTokenValidator& operator=(const AppleTokenValidator&) = delete;
    AppleTokenValidator(AppleTokenValidator&&) noexcept;
    AppleTokenValidator& operator=(AppleTokenValidator&&) noexcept;

    VerifyResult verify_token(const std::string& id_token);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}
}

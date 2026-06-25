#pragma once

#include "compat/cxx11.hpp"

#include <memory>
#include <string>

namespace mirobody { namespace jwt {

// Verifier for Google ID tokens (the RS256 JWT issued by Google after a
// successful sign-in flow). Mirrors the Python sibling at
// a007-opensource/mirobody/user/google.py:
//   - Pulls Google's JWKS from https://www.googleapis.com/oauth2/v3/certs
//     on first use; caches the parsed keys for the response's
//     Cache-Control max-age (clamped to [60s, 24h]; 1 hour fallback when
//     the header is missing or unparseable).
//   - Picks the right key per token by the JWT header's `kid` claim,
//     refreshing the cache once if the kid isn't known (Google rotates
//     keys before the cache TTL).
//   - Verifies the RS256 signature, the `iss` (must be
//     `https://accounts.google.com` or the legacy `accounts.google.com`),
//     the `aud` (must match the configured client_id), the `exp` (must
//     be in the future), and asserts the payload carries an `email`
//     claim.
//
// Thread-safe: the cache is protected by an internal mutex.
class GoogleTokenValidator {
public:
    static constexpr const char* kGoogleJwkUrl = "https://www.googleapis.com/oauth2/v3/certs";
    static constexpr const char* kGoogleIssuer = "https://accounts.google.com";

    struct VerifyResult {
        std::string claims_json;   // payload JSON when ok() is true
        std::string error;
        bool ok() const { return error.empty(); }
    };

    // `client_id` is the OAuth 2.0 client ID that issued the token: it
    // must match the token's `aud` claim or verification fails.
    explicit GoogleTokenValidator(std::string client_id);
    ~GoogleTokenValidator();

    GoogleTokenValidator(const GoogleTokenValidator&)            = delete;
    GoogleTokenValidator& operator=(const GoogleTokenValidator&) = delete;
    GoogleTokenValidator(GoogleTokenValidator&&) noexcept;
    GoogleTokenValidator& operator=(GoogleTokenValidator&&) noexcept;

    VerifyResult verify_token(const std::string& id_token);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}
}

#pragma once

#include "compat/cxx11.hpp"

#include <memory>
#include <string>

namespace mirobody { namespace jwt {

// Verifier for Firebase Auth ID tokens (the RS256 JWT issued after a
// successful Firebase sign-in). Mirrors the Python sibling at
// a007-opensource/mirobody/user/firebase.py:
//   - Pulls Google's `securetoken@system.gserviceaccount.com` cert bundle
//     from https://www.googleapis.com/robot/v1/metadata/x509/...; the
//     response is a flat `{kid: PEM-X509-cert}` map, not a JWK set, so
//     each value is parsed as an X.509 certificate and its public key
//     extracted before signature verification.
//   - Caches the parsed keys for one hour. The Python sibling honors
//     the response's `Cache-Control: max-age=` instead; HttpClient does
//     not surface response headers, so we use the Python default.
//   - Picks the right key per token by the JWT header's `kid` claim,
//     refreshing the cache once if the kid isn't known.
//   - Verifies the RS256 signature, the `iss` (must be
//     `https://securetoken.google.com/<project_id>`), the `aud` (must
//     match the configured `project_id`), the `exp` (must be in the
//     future), and asserts the payload carries an `email` claim.
//
// Thread-safe: the cache is protected by an internal mutex.
class FirebaseTokenValidator {
public:
    static constexpr const char* kFirebaseJwkUrl =
        "https://www.googleapis.com/robot/v1/metadata/x509/securetoken@system.gserviceaccount.com";

    struct VerifyResult {
        std::string claims_json;   // payload JSON when ok() is true
        std::string error;
        bool ok() const { return error.empty(); }
    };

    // `project_id` is the Firebase project ID: it must match the token's
    // `aud` claim and forms the `iss` claim
    // `https://securetoken.google.com/<project_id>`.
    explicit FirebaseTokenValidator(std::string project_id);
    ~FirebaseTokenValidator();

    FirebaseTokenValidator(const FirebaseTokenValidator&)            = delete;
    FirebaseTokenValidator& operator=(const FirebaseTokenValidator&) = delete;
    FirebaseTokenValidator(FirebaseTokenValidator&&) noexcept;
    FirebaseTokenValidator& operator=(FirebaseTokenValidator&&) noexcept;

    VerifyResult verify_token(const std::string& id_token);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}
}

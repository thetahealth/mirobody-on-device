#pragma once

#include "compat/cxx11.hpp"

#include <memory>
#include <string>
#include <vector>

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
//     match a configured project id), the `exp` (must be in the
//     future), and asserts the payload carries an `email` claim.
//
// SEVERAL project ids may be accepted. Different clients can be built against
// different Firebase projects -- the Android APK ships a google-services.json
// naming one, the web client is handed another by the server -- and a token is
// only ever valid for the project that issued it. Accepting a set lets one
// deployment serve both without every client being rebuilt against one project.
// It costs no extra key fetching: the cert bundle above is Google-wide, not
// per-project; only the `aud` / `iss` comparison widens.
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

    // Each project id must match the token's `aud` claim and forms an accepted
    // `iss` claim `https://securetoken.google.com/<project_id>`; a token is
    // valid when it matches ANY of them. The single-argument form is the common
    // case and is kept so existing callers and tests read unchanged. Empty ids
    // are dropped; constructing with none accepts nothing.
    explicit FirebaseTokenValidator(std::string project_id);
    explicit FirebaseTokenValidator(std::vector<std::string> project_ids);
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

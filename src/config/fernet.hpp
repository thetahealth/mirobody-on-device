#pragma once

#include <array>
#include <cstdint>
#include <stdexcept>
#include <string>
#include "compat/cxx11.hpp"

namespace mirobody { namespace encrypt {

class FernetError : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

// Symmetric authenticated encryption per the Fernet spec
// (https://github.com/fernet/spec/blob/master/Spec.md). Wire-compatible with
// Python's cryptography.fernet.Fernet — tokens produced by either side decrypt
// on the other given the same 32-byte key.
class Fernet {
public:
    // url_safe_b64_key: URL-safe base64 of a 32-byte key (44 chars including
    // '=' padding). This matches the format Fernet.generate_key() emits in
    // Python. Throws FernetError on malformed input.
    explicit Fernet(const std::string& url_safe_b64_key);

    // Generate a fresh random key as a 44-char URL-safe base64 string.
    static std::string generate_key();

    // Encrypt with a fresh random IV; returns a URL-safe base64 Fernet token.
    std::string encrypt(const std::string& plaintext) const;

    // Encrypt with a caller-supplied timestamp (seconds since the UNIX epoch).
    // Useful for tests and for emitting tokens with a specific issued-at time.
    // Mirrors Python's Fernet.encrypt_at_time.
    std::string encrypt_at(const std::string& plaintext, std::int64_t timestamp) const;

    // Decrypt a token. If ttl_seconds > 0, reject tokens older than that
    // (with a 60s tolerance for clock skew on future-dated tokens, matching
    // Python's MAX_CLOCK_SKEW). Throws FernetError on auth failure, expiry,
    // or malformed input.
    std::string decrypt(const std::string& token, std::int64_t ttl_seconds = 0) const;

private:
    std::array<std::uint8_t, 16> signing_key_{};
    std::array<std::uint8_t, 16> encryption_key_{};
};

}
}

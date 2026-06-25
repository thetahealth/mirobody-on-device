#pragma once

#include "compat/cxx11.hpp"

#include <cstdint>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace mirobody { namespace jwt {

// Project-wide JWT claim defaults. Held in a single place so JwtHs256
// and JwtRs256 (and any future derivation) don't drift apart. Values
// mirror the Python sibling at a007-opensource/mirobody/user/jwt.py.
constexpr const char* kDefaultIss       = "mb_oauth";
constexpr const char* kDefaultAud       = "mb";
constexpr const char* kDefaultClientId  = "mb_c";
constexpr const char* kDefaultScope     = "mcp:read mcp:write";
constexpr std::int64_t kDefaultExpiresIn = 60 * 60 * 24 * 30;   // seconds (30 days)

//------------------------------------------------------------------------------

// Abstract JWT base. Owns the claim-set + token framing (header,
// payload, base64url encoding, exp policy) and defers the
// algorithm-specific bits (header `alg` value, signature production,
// signature verification) to derived classes. See JwtHs256 / JwtRs256.
//
// Defaults for iss / aud / client_id / scope / expires_in mirror the
// Python sibling at a007-opensource/mirobody/user/jwt.py.
class Jwt {
public:
    struct VerifyResult {
        std::string  claims_json;   // payload JSON when ok() is true
        std::string  error;         // failure reason; empty on success
        std::int64_t user_id = 0;   // subject decoded back to the row id; <=0 if
                                    // absent / not decodable (e.g. wrong salt)
        bool ok() const { return error.empty(); }
    };

    virtual ~Jwt() = default;

    // Build and sign a JWT for `user_id`. The token subject is the opaque
    // encoding of the row id (encrypt::encode_int64(user_id, salt())) -- callers
    // pass the raw id, not a pre-encoded subject; verifiers decode it back with
    // the same salt. `extra` overlays the default claim set (later writes win).
    // When `expires_in > 60` it overrides the configured default; otherwise the
    // constructed expires_in is used (matching Python's pass-0-for-default
    // sentinel).
    std::string generate(
        std::int64_t user_id,
        const std::unordered_map<std::string, std::string>& extra = {},
        std::int64_t expires_in = 0) const;

    // Verify signature + `exp`. iss / aud are NOT verified,
    // matching the Python (verify_iss=False / verify_aud=False). A leading
    // "Bearer " scheme prefix, if present, is stripped — so a raw HTTP
    // Authorization header value can be passed directly.
    VerifyResult verify(const std::string& token) const;

    // Like verify() but allows `exp` to be in the past, up to `max_age`
    // seconds. For session re-auth where the token has expired due to
    // idle timeout but the user can still re-auth within a grace
    // period.
    VerifyResult verify_allow_expired(const std::string& token,
                                      std::int64_t max_age = 86400) const;

    // Configured default token lifetime in seconds — the `expires_in` value
    // reported to clients after login. Mirrors the Python get_expires_in().
    std::int64_t expires_in() const { return expires_in_; }

    // Salt (JWT_SALT) used to obfuscate the token subject: the row id is run
    // through encrypt::encode_int64(id, salt()) at issuance and decoded back on
    // verification. Held here so every holder of a Jwt has the salt without
    // threading it separately. Empty when JWT_SALT is unset.
    const std::string& salt() const { return salt_; }

protected:
    Jwt(std::string iss, std::string aud, std::string client_id,
        std::string scope, std::int64_t expires_in, std::string salt);

    // Algorithm name placed in the JWT header (`HS256`, `RS256`, ...).
    virtual std::string algorithm() const = 0;

    // Key id placed in the JWT header `kid` when non-empty (so a verifier that
    // holds a JWKS can select the right key). Default empty — HS256 has no
    // published key, so it emits no `kid`; JwtRs256 overrides this with its
    // JWK thumbprint.
    virtual std::string key_id() const { return std::string(); }

    // Produce the raw (un-base64-encoded) signature bytes for
    // `<header_b64>.<payload_b64>`. Throws on misconfiguration (e.g.,
    // no signing key on JwtRs256).
    virtual std::string sign(const std::string& signing_input) const = 0;

    // Whether `signature` (raw, already base64-decoded) is the valid
    // signature for `signing_input`. Returns false on any failure,
    // including "no verifying key configured".
    virtual bool verify_signature(const std::string& signing_input,
                                  const std::string& signature) const = 0;

    std::string  iss_;
    std::string  aud_;
    std::string  client_id_;
    std::string  scope_;
    std::int64_t expires_in_;
    std::string  salt_;

private:
    VerifyResult do_verify(const std::string& token,
                           bool verify_exp,
                           std::int64_t allow_expired_max_age) const;
};

//------------------------------------------------------------------------------

// HS256 (symmetric) JWT validator. A single secret key signs and
// verifies; suitable for first-party use where both sides hold the
// same secret.
class JwtHs256 : public Jwt {
public:
    struct Options {
        std::string  key;
        std::string  iss        = kDefaultIss;
        std::string  aud        = kDefaultAud;
        std::string  client_id  = kDefaultClientId;
        std::string  scope      = kDefaultScope;
        std::int64_t expires_in = kDefaultExpiresIn;
        std::string  salt;       // JWT_SALT; keys the subject obfuscation
    };

    // Throws std::runtime_error when `key` is empty.
    explicit JwtHs256(Options opts);

protected:
    std::string algorithm() const override;
    std::string sign(const std::string& signing_input) const override;
    bool verify_signature(const std::string& signing_input,
                          const std::string& signature) const override;

private:
    std::string key_;
};

//------------------------------------------------------------------------------

// RS256 (RSASSA-PKCS1-v1_5 with SHA-256) JWT validator. The private key
// signs, the public key verifies. Either side may be omitted: pass only
// `private_key_pem` for sign-only, or only `public_key_pem` for
// verify-only (e.g., a service that consumes third-party tokens).
//
// PEM formats accepted:
//   - private_key_pem: PKCS#1 (`-----BEGIN RSA PRIVATE KEY-----`) or
//     PKCS#8 unencrypted (`-----BEGIN PRIVATE KEY-----`).
//   - public_key_pem:  SubjectPublicKeyInfo (`-----BEGIN PUBLIC KEY-----`),
//     which is what `openssl rsa -pubout` and JWKS derivations produce.
class JwtRs256 : public Jwt {
public:
    struct Options {
        std::string  private_key_pem;   // optional if verify-only
        std::string  public_key_pem;    // optional if sign-only
        // Extra verify-only public-key PEMs (key rotation). They are added to the
        // verify set and published in the JWKS, but never used for signing — the
        // token is always signed with private_key_pem. See the README.
        std::vector<std::string> additional_public_key_pems;
        std::string  iss        = kDefaultIss;
        std::string  aud        = kDefaultAud;
        std::string  client_id  = kDefaultClientId;
        std::string  scope      = kDefaultScope;
        std::int64_t expires_in = kDefaultExpiresIn;
        std::string  salt;       // JWT_SALT; keys the subject obfuscation
    };

    // Throws std::runtime_error when a provided PEM blob fails to parse.
    // Construction with both PEMs empty is allowed (the resulting object
    // can neither sign nor verify, which is useless but not a bug).
    explicit JwtRs256(Options opts);
    ~JwtRs256() override;

    JwtRs256(const JwtRs256&)            = delete;
    JwtRs256& operator=(const JwtRs256&) = delete;
    JwtRs256(JwtRs256&&) noexcept;
    JwtRs256& operator=(JwtRs256&&) noexcept;

    // RFC 7638 JWK thumbprint of the key, emitted as the token header `kid` and
    // as the `kid` of the published JWK so a verifier can match them. Empty if
    // no usable key was supplied (neither PEM parsed into RSA params).
    const std::string& kid() const;

    // The public key as a JWKS document: {"keys":[{kty,use,alg,kid,n,e}]}.
    // Suitable to serve verbatim at /.well-known/jwks.json for third-party
    // validators. Empty when no usable public key is available.
    const std::string& jwks_json() const;

protected:
    std::string algorithm() const override;
    std::string key_id() const override;
    std::string sign(const std::string& signing_input) const override;
    bool verify_signature(const std::string& signing_input,
                          const std::string& signature) const override;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

// Derive the SubjectPublicKeyInfo PEM (`-----BEGIN PUBLIC KEY-----`) from an
// RSA/EC private-key PEM. Lets a deployment configure only the private key and
// still publish the matching public key (JWKS). Returns "" if the PEM does not
// parse.
std::string rsa_public_pem_from_private(const std::string& private_pem);

}
}

#pragma once


#include <cstddef>
#include <string>

namespace mirobody { namespace oauth {

// Small crypto / encoding helpers shared by the OAuth 2.0 authorization server.
// All base64url is RFC 4648 sec.5 (URL/filename-safe alphabet) with NO padding,
// the encoding OAuth/JWT/PKCE all use on the wire.

// Encode `n` bytes at `data` as unpadded base64url.
std::string base64url_encode(const unsigned char* data, std::size_t n);
inline std::string base64url_encode(const std::string& s) {
    return base64url_encode(reinterpret_cast<const unsigned char*>(s.data()), s.size());
}

// Decode an unpadded (padding tolerated) base64url string into raw bytes.
// Returns false on any invalid character or truncated group.
bool base64url_decode(const std::string& in, std::string* out);

// `count` cryptographically-random bytes rendered as unpadded base64url. Used
// to mint client ids, authorization codes, and the request-handle nonces.
// Falls back to std::rand only if the OpenSSL CSPRNG is unavailable.
std::string random_token(std::size_t bytes = 32);

// Raw SHA-256 digest (32 bytes) of `in`.
std::string sha256(const std::string& in);

// PKCE S256 verification (RFC 7636 sec.4.6): true iff
//   base64url(SHA256(code_verifier)) == code_challenge.
// Comparison is constant-time. Returns false if either input is empty.
bool verify_pkce_s256(const std::string& code_verifier,
                      const std::string& code_challenge);

}}

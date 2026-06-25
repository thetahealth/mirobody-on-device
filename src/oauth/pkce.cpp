#include "oauth/pkce.hpp"

#include <openssl/rand.h>
#include <openssl/sha.h>

#include <cstdlib>

namespace mirobody { namespace oauth {

namespace {

const char kAlphabet[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";

// Reverse map for base64url decode: index by byte -> 0..63, or 0xFF if not a
// base64url symbol. '=' padding is treated as a terminator by the decoder.
signed char decode_value(unsigned char c) {
    if (c >= 'A' && c <= 'Z') return static_cast<signed char>(c - 'A');
    if (c >= 'a' && c <= 'z') return static_cast<signed char>(c - 'a' + 26);
    if (c >= '0' && c <= '9') return static_cast<signed char>(c - '0' + 52);
    if (c == '-') return 62;
    if (c == '_') return 63;
    return -1;
}

}  // namespace

std::string base64url_encode(const unsigned char* data, std::size_t n) {
    std::string out;
    out.reserve((n + 2) / 3 * 4);
    std::size_t i = 0;
    for (; i + 3 <= n; i += 3) {
        unsigned v = (static_cast<unsigned>(data[i]) << 16) |
                     (static_cast<unsigned>(data[i + 1]) << 8) |
                      static_cast<unsigned>(data[i + 2]);
        out += kAlphabet[(v >> 18) & 63];
        out += kAlphabet[(v >> 12) & 63];
        out += kAlphabet[(v >> 6) & 63];
        out += kAlphabet[v & 63];
    }
    const std::size_t rem = n - i;
    if (rem == 1) {
        unsigned v = static_cast<unsigned>(data[i]) << 16;
        out += kAlphabet[(v >> 18) & 63];
        out += kAlphabet[(v >> 12) & 63];
    } else if (rem == 2) {
        unsigned v = (static_cast<unsigned>(data[i]) << 16) |
                     (static_cast<unsigned>(data[i + 1]) << 8);
        out += kAlphabet[(v >> 18) & 63];
        out += kAlphabet[(v >> 12) & 63];
        out += kAlphabet[(v >> 6) & 63];
    }
    return out;
}

bool base64url_decode(const std::string& in, std::string* out) {
    out->clear();
    // Drop any trailing '=' padding (tolerated though our encoder omits it).
    std::size_t len = in.size();
    while (len > 0 && in[len - 1] == '=') --len;

    unsigned buf = 0;
    int bits = 0;
    for (std::size_t i = 0; i < len; ++i) {
        const signed char v = decode_value(static_cast<unsigned char>(in[i]));
        if (v < 0) return false;
        buf = (buf << 6) | static_cast<unsigned>(v);
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            out->push_back(static_cast<char>((buf >> bits) & 0xFF));
        }
    }
    // A valid stream leaves only the discarded low bits, and those must be zero.
    if (bits >= 6) return false;             // a lone 6-bit group is malformed
    if (bits > 0 && (buf & ((1u << bits) - 1)) != 0) return false;
    return true;
}

std::string random_token(std::size_t bytes) {
    std::string raw(bytes, '\0');
    unsigned char* p = reinterpret_cast<unsigned char*>(&raw[0]);
    if (RAND_bytes(p, static_cast<int>(bytes)) != 1) {
        for (std::size_t i = 0; i < bytes; ++i) {
            p[i] = static_cast<unsigned char>(std::rand() & 0xFF);
        }
    }
    return base64url_encode(p, bytes);
}

std::string sha256(const std::string& in) {
    unsigned char digest[SHA256_DIGEST_LENGTH];
    SHA256(reinterpret_cast<const unsigned char*>(in.data()), in.size(), digest);
    return std::string(reinterpret_cast<const char*>(digest), sizeof(digest));
}

bool verify_pkce_s256(const std::string& code_verifier,
                      const std::string& code_challenge) {
    if (code_verifier.empty() || code_challenge.empty()) return false;
    const std::string expected = base64url_encode(sha256(code_verifier));
    // Constant-time compare to avoid leaking the challenge via timing.
    if (expected.size() != code_challenge.size()) return false;
    unsigned char diff = 0;
    for (std::size_t i = 0; i < expected.size(); ++i) {
        diff |= static_cast<unsigned char>(expected[i] ^ code_challenge[i]);
    }
    return diff == 0;
}

}}

#pragma once

// Reversible, authenticated encoding of a database row id (uint32 range),
// keyed by a string salt (config.jwt.salt / JWT_SALT).
//
//   encode_int64(id, salt) -> 16-char URL-safe token
//   decode_int64(token, salt) -> id, or -1 if the token is forged/corrupt
//
// Properties:
//   * Unforgeable. The token carries a 64-bit SipHash tag over the ciphertext,
//     keyed by the salt. Producing a token for an id you weren't given means
//     guessing that tag (2^-64) or breaking SipHash — infeasible for a
//     determined attacker. A tampered token fails the check and decodes to -1.
//   * Hidden. The id is run through a 4-round Feistel network (a strong PRP,
//     Luby-Rackoff) whose round function is keyed SipHash, so the token reveals
//     nothing about the id and consecutive ids look unrelated.
//   * Deterministic. The same (id, salt) always yields the same token, so it is
//     stable in URLs. Requires 0 <= id < 2^32 (encode returns "" otherwise).
//
// This is integrity + confidentiality of the id, not authorization: a decoded
// id must still be looked up and the caller checked against it.
//
// SipHash-2-4 is the reference construction by Aumasson & Bernstein.

#include <cstddef>
#include <cstdint>
#include <string>

namespace mirobody { namespace encrypt {

namespace detail {

//------------------------------------------------------------------------------
// SipHash-2-4 — a keyed 64-bit PRF used for both the Feistel round function and
// the integrity tag (domain-separated by a leading byte on the input).
//------------------------------------------------------------------------------

inline std::uint64_t sip_rotl(std::uint64_t x, int b) {
    return (x << b) | (x >> (64 - b));
}

inline void sip_round(std::uint64_t& v0, std::uint64_t& v1,
                      std::uint64_t& v2, std::uint64_t& v3) {
    v0 += v1; v1 = sip_rotl(v1, 13); v1 ^= v0; v0 = sip_rotl(v0, 32);
    v2 += v3; v3 = sip_rotl(v3, 16); v3 ^= v2;
    v0 += v3; v3 = sip_rotl(v3, 21); v3 ^= v0;
    v2 += v1; v1 = sip_rotl(v1, 17); v1 ^= v2; v2 = sip_rotl(v2, 32);
}

inline std::uint64_t siphash24(const unsigned char* in, std::size_t inlen,
                               std::uint64_t k0, std::uint64_t k1) {
    std::uint64_t v0 = 0x736f6d6570736575ULL ^ k0;
    std::uint64_t v1 = 0x646f72616e646f6dULL ^ k1;
    std::uint64_t v2 = 0x6c7967656e657261ULL ^ k0;
    std::uint64_t v3 = 0x7465646279746573ULL ^ k1;

    const std::size_t whole = inlen - (inlen % 8);
    std::uint64_t b = static_cast<std::uint64_t>(inlen) << 56;

    std::size_t i = 0;
    for (; i < whole; i += 8) {
        std::uint64_t m = 0;
        for (int j = 0; j < 8; ++j) m |= static_cast<std::uint64_t>(in[i + j]) << (8 * j);
        v3 ^= m; sip_round(v0, v1, v2, v3); sip_round(v0, v1, v2, v3); v0 ^= m;
    }
    for (std::size_t j = 0; i + j < inlen; ++j) {
        b |= static_cast<std::uint64_t>(in[i + j]) << (8 * j);
    }
    v3 ^= b; sip_round(v0, v1, v2, v3); sip_round(v0, v1, v2, v3); v0 ^= b;
    v2 ^= 0xff;
    sip_round(v0, v1, v2, v3); sip_round(v0, v1, v2, v3);
    sip_round(v0, v1, v2, v3); sip_round(v0, v1, v2, v3);
    return v0 ^ v1 ^ v2 ^ v3;
}

// Stretch the salt into a 128-bit working key (two independent SipHash keys).
inline void derive_key(const std::string& salt, std::uint64_t& wk0, std::uint64_t& wk1) {
    const unsigned char* p = reinterpret_cast<const unsigned char*>(salt.data());
    wk0 = siphash24(p, salt.size(), 0x0706050403020100ULL, 0x0f0e0d0c0b0a0908ULL);
    wk1 = siphash24(p, salt.size(), 0x1f1e1d1c1b1a1918ULL, 0x2726252423222120ULL);
}

//------------------------------------------------------------------------------
// 4-round Feistel over the 32-bit id (two 16-bit halves). 0x46 ('F') tags the
// round-function input so it can't collide with the integrity tag's input.
//------------------------------------------------------------------------------

const int kRounds = 4;

inline std::uint16_t feistel_f(int round, std::uint16_t r,
                               std::uint64_t wk0, std::uint64_t wk1) {
    const unsigned char buf[4] = {
        0x46,
        static_cast<unsigned char>(round),
        static_cast<unsigned char>(r & 0xff),
        static_cast<unsigned char>((r >> 8) & 0xff),
    };
    return static_cast<std::uint16_t>(siphash24(buf, sizeof(buf), wk0, wk1) & 0xffff);
}

inline std::uint32_t feistel_encrypt(std::uint32_t v, std::uint64_t wk0, std::uint64_t wk1) {
    std::uint16_t l = static_cast<std::uint16_t>(v >> 16);
    std::uint16_t r = static_cast<std::uint16_t>(v & 0xffff);
    for (int round = 0; round < kRounds; ++round) {
        const std::uint16_t nl = r;
        const std::uint16_t nr = static_cast<std::uint16_t>(l ^ feistel_f(round, r, wk0, wk1));
        l = nl; r = nr;
    }
    return (static_cast<std::uint32_t>(l) << 16) | r;
}

inline std::uint32_t feistel_decrypt(std::uint32_t v, std::uint64_t wk0, std::uint64_t wk1) {
    std::uint16_t l = static_cast<std::uint16_t>(v >> 16);
    std::uint16_t r = static_cast<std::uint16_t>(v & 0xffff);
    for (int round = kRounds - 1; round >= 0; --round) {
        const std::uint16_t r0 = l;                          // the pre-round R
        const std::uint16_t l0 = static_cast<std::uint16_t>(r ^ feistel_f(round, r0, wk0, wk1));
        l = l0; r = r0;
    }
    return (static_cast<std::uint32_t>(l) << 16) | r;
}

// 64-bit integrity tag over the 4-byte ciphertext. 0x4d ('M') domain-separates.
inline std::uint64_t tag_of(std::uint32_t ct, std::uint64_t wk0, std::uint64_t wk1) {
    const unsigned char buf[5] = {
        0x4d,
        static_cast<unsigned char>((ct >> 24) & 0xff),
        static_cast<unsigned char>((ct >> 16) & 0xff),
        static_cast<unsigned char>((ct >> 8) & 0xff),
        static_cast<unsigned char>(ct & 0xff),
    };
    return siphash24(buf, sizeof(buf), wk0, wk1);
}

//------------------------------------------------------------------------------
// base64url (no padding). Inputs here are a fixed 12 bytes -> 16 chars.
//------------------------------------------------------------------------------

const char kB64[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";

inline int b64_value(char c) {
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    if (c == '-') return 62;
    if (c == '_') return 63;
    return -1;
}

inline std::string b64url_encode(const unsigned char* p, std::size_t n) {
    std::string out;
    out.reserve((n / 3) * 4);
    for (std::size_t i = 0; i < n; i += 3) {
        const std::uint32_t v = (static_cast<std::uint32_t>(p[i]) << 16) |
                                (static_cast<std::uint32_t>(p[i + 1]) << 8) |
                                 static_cast<std::uint32_t>(p[i + 2]);
        out.push_back(kB64[(v >> 18) & 63]);
        out.push_back(kB64[(v >> 12) & 63]);
        out.push_back(kB64[(v >> 6) & 63]);
        out.push_back(kB64[v & 63]);
    }
    return out;
}

// Decode exactly `n` bytes (n a multiple of 3) from n/3*4 chars. False on any
// length/alphabet violation.
inline bool b64url_decode(const std::string& s, unsigned char* out, std::size_t n) {
    if (s.size() != (n / 3) * 4) return false;
    std::size_t oi = 0;
    for (std::size_t i = 0; i < s.size(); i += 4) {
        const int a = b64_value(s[i]);
        const int b = b64_value(s[i + 1]);
        const int c = b64_value(s[i + 2]);
        const int d = b64_value(s[i + 3]);
        if ((a | b | c | d) < 0) return false;
        const std::uint32_t v = (static_cast<std::uint32_t>(a) << 18) |
                                (static_cast<std::uint32_t>(b) << 12) |
                                (static_cast<std::uint32_t>(c) << 6) |
                                 static_cast<std::uint32_t>(d);
        out[oi++] = static_cast<unsigned char>((v >> 16) & 0xff);
        out[oi++] = static_cast<unsigned char>((v >> 8) & 0xff);
        out[oi++] = static_cast<unsigned char>(v & 0xff);
    }
    return true;
}

}   // namespace detail

// Encode `n` (must be 0 <= n < 2^32) into a 16-char unforgeable token keyed by
// `salt`. Returns "" if `n` is out of the supported range.
inline std::string encode_int64(std::int64_t n, std::string salt) {
    if (n < 0 || n > 0xFFFFFFFFLL) return std::string();

    std::uint64_t wk0, wk1;
    detail::derive_key(salt, wk0, wk1);

    const std::uint32_t ct  = detail::feistel_encrypt(static_cast<std::uint32_t>(n), wk0, wk1);
    const std::uint64_t tag = detail::tag_of(ct, wk0, wk1);

    unsigned char raw[12];
    raw[0] = static_cast<unsigned char>((ct >> 24) & 0xff);
    raw[1] = static_cast<unsigned char>((ct >> 16) & 0xff);
    raw[2] = static_cast<unsigned char>((ct >> 8) & 0xff);
    raw[3] = static_cast<unsigned char>(ct & 0xff);
    for (int i = 0; i < 8; ++i) {
        raw[4 + i] = static_cast<unsigned char>((tag >> (56 - 8 * i)) & 0xff);
    }
    return detail::b64url_encode(raw, sizeof(raw));
}

// Inverse of encode_int64 for the same `salt`. Returns the id, or -1 when the
// token is malformed or its tag does not verify (forged / corrupted / wrong
// salt). Valid ids are >= 0, so -1 is an unambiguous failure sentinel.
inline std::int64_t decode_int64(std::string s, std::string salt) {
    unsigned char raw[12];
    if (!detail::b64url_decode(s, raw, sizeof(raw))) return -1;

    const std::uint32_t ct = (static_cast<std::uint32_t>(raw[0]) << 24) |
                             (static_cast<std::uint32_t>(raw[1]) << 16) |
                             (static_cast<std::uint32_t>(raw[2]) << 8) |
                              static_cast<std::uint32_t>(raw[3]);
    std::uint64_t tag = 0;
    for (int i = 0; i < 8; ++i) tag = (tag << 8) | raw[4 + i];

    std::uint64_t wk0, wk1;
    detail::derive_key(salt, wk0, wk1);

    if (tag != detail::tag_of(ct, wk0, wk1)) return -1;   // forged / corrupt / wrong salt

    return static_cast<std::int64_t>(detail::feistel_decrypt(ct, wk0, wk1));
}

}}

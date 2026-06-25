#include "config/fernet.hpp"

#include <openssl/crypto.h>
#include <openssl/evp.h>
#include <openssl/rand.h>

#include <chrono>
#include <cstring>
#include <vector>

namespace mirobody { namespace encrypt {

namespace {

constexpr std::uint8_t  kVersion = 0x80;
constexpr std::size_t   kIvSize = 16;
constexpr std::size_t   kHmacSize = 32;
constexpr std::size_t   kHeaderSize = 1 + 8 + kIvSize;
constexpr std::size_t   kKeySize = 32;
constexpr std::int64_t  kMaxClockSkewSeconds = 60;

constexpr char kAlphabet[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZ"
    "abcdefghijklmnopqrstuvwxyz"
    "0123456789-_";

//------------------------------------------------------------------------------

const std::array<std::uint8_t, 256>& decode_table() {
    static const auto t = []{
        std::array<std::uint8_t, 256> a{};
        a.fill(0xFF);
        for (std::uint8_t i = 0; i < 64; ++i) a[static_cast<std::uint8_t>(kAlphabet[i])] = i;
        return a;
    }();
    return t;
}

//------------------------------------------------------------------------------

std::string b64url_encode(const std::uint8_t* data, std::size_t len) {
    std::string out;
    out.reserve(((len + 2) / 3) * 4);
    for (std::size_t i = 0; i < len; i += 3) {
        std::uint32_t n = static_cast<std::uint32_t>(data[i]) << 16;
        std::size_t take = 1;
        if (i + 1 < len) { n |= static_cast<std::uint32_t>(data[i + 1]) << 8; take = 2; }
        if (i + 2 < len) { n |= static_cast<std::uint32_t>(data[i + 2]);      take = 3; }
        out.push_back(kAlphabet[(n >> 18) & 0x3F]);
        out.push_back(kAlphabet[(n >> 12) & 0x3F]);
        out.push_back(take >= 2 ? kAlphabet[(n >> 6) & 0x3F] : '=');
        out.push_back(take >= 3 ? kAlphabet[n & 0x3F]        : '=');
    }
    return out;
}

//------------------------------------------------------------------------------

std::vector<std::uint8_t> b64url_decode(const std::string& s) {
    std::size_t len = s.size();
    while (len > 0 && s[len - 1] == '=') --len;
    const auto& tbl = decode_table();
    std::vector<std::uint8_t> out;
    out.reserve((len * 3) / 4);
    std::uint32_t n = 0;
    int bits = 0;
    for (std::size_t i = 0; i < len; ++i) {
        std::uint8_t v = tbl[static_cast<std::uint8_t>(s[i])];
        if (v == 0xFF) throw FernetError("invalid base64 in token");
        n = (n << 6) | v;
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            out.push_back(static_cast<std::uint8_t>((n >> bits) & 0xFF));
        }
    }
    return out;
}

//------------------------------------------------------------------------------

std::array<std::uint8_t, 32> hmac_sha256(const std::uint8_t* key, std::size_t key_len,
                                         const std::uint8_t* data, std::size_t data_len) {
    std::array<std::uint8_t, 32> out{};
    std::size_t out_len = 0;
    // EVP_Q_mac is the one-shot replacement for the deprecated HMAC() helper
    // (OpenSSL 3.0+). It allocates and frees the EVP_MAC context internally.
    auto* ok = EVP_Q_mac(nullptr, "HMAC", nullptr, "SHA256", nullptr,
                         key, key_len, data, data_len,
                         out.data(), out.size(), &out_len);
    if (ok == nullptr || out_len != 32) throw FernetError("HMAC-SHA256 failed");
    return out;
}

//------------------------------------------------------------------------------

std::int64_t now_seconds() {
    return std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count();
}

//------------------------------------------------------------------------------

void write_be64(std::uint8_t* p, std::int64_t v) {
    auto u = static_cast<std::uint64_t>(v);
    for (int i = 7; i >= 0; --i) { p[i] = static_cast<std::uint8_t>(u & 0xFF); u >>= 8; }
}

std::int64_t read_be64(const std::uint8_t* p) {
    std::uint64_t u = 0;
    for (int i = 0; i < 8; ++i) u = (u << 8) | p[i];
    return static_cast<std::int64_t>(u);
}

//------------------------------------------------------------------------------

struct CipherCtx {
    EVP_CIPHER_CTX* ctx;
    CipherCtx() : ctx(EVP_CIPHER_CTX_new()) {
        if (!ctx) throw FernetError("EVP_CIPHER_CTX_new failed");
    }
    ~CipherCtx() { if (ctx) EVP_CIPHER_CTX_free(ctx); }
    CipherCtx(const CipherCtx&) = delete;
    CipherCtx& operator=(const CipherCtx&) = delete;
};

}

//------------------------------------------------------------------------------

Fernet::Fernet(const std::string& url_safe_b64_key) {
    auto key = b64url_decode(url_safe_b64_key);
    if (key.size() != kKeySize) throw FernetError("fernet key must decode to 32 bytes");
    std::memcpy(signing_key_.data(),    key.data(),      16);
    std::memcpy(encryption_key_.data(), key.data() + 16, 16);
    OPENSSL_cleanse(key.data(), key.size());
}

//------------------------------------------------------------------------------

std::string Fernet::generate_key() {
    std::array<std::uint8_t, kKeySize> raw{};
    if (RAND_bytes(raw.data(), static_cast<int>(raw.size())) != 1) throw FernetError("RAND_bytes failed");
    auto s = b64url_encode(raw.data(), raw.size());
    OPENSSL_cleanse(raw.data(), raw.size());
    return s;
}

//------------------------------------------------------------------------------

std::string Fernet::encrypt(const std::string& plaintext) const {
    return encrypt_at(plaintext, now_seconds());
}

//------------------------------------------------------------------------------

std::string Fernet::encrypt_at(const std::string& plaintext, std::int64_t timestamp) const {
    std::uint8_t iv[kIvSize];
    if (RAND_bytes(iv, kIvSize) != 1) throw FernetError("RAND_bytes failed");

    // AES-128-CBC with PKCS7 padding; ciphertext is up to plaintext.size() + 16.
    std::vector<std::uint8_t> ct(plaintext.size() + kIvSize);
    int len_out = 0, total = 0;

    CipherCtx g;
    if (EVP_EncryptInit_ex(g.ctx, EVP_aes_128_cbc(), nullptr, encryption_key_.data(), iv) != 1)
        throw FernetError("EVP_EncryptInit_ex failed");
    if (EVP_EncryptUpdate(g.ctx, ct.data(), &len_out,
                          reinterpret_cast<const std::uint8_t*>(plaintext.data()),
                          static_cast<int>(plaintext.size())) != 1)
        throw FernetError("EVP_EncryptUpdate failed");
    total = len_out;
    if (EVP_EncryptFinal_ex(g.ctx, ct.data() + total, &len_out) != 1)
        throw FernetError("EVP_EncryptFinal_ex failed");
    total += len_out;
    ct.resize(static_cast<std::size_t>(total));

    std::vector<std::uint8_t> body;
    body.reserve(kHeaderSize + ct.size() + kHmacSize);
    body.push_back(kVersion);
    std::uint8_t ts[8];
    write_be64(ts, timestamp);
    body.insert(body.end(), ts, ts + 8);
    body.insert(body.end(), iv, iv + kIvSize);
    body.insert(body.end(), ct.begin(), ct.end());

    auto mac = hmac_sha256(signing_key_.data(), signing_key_.size(), body.data(), body.size());
    body.insert(body.end(), mac.begin(), mac.end());

    return b64url_encode(body.data(), body.size());
}

//------------------------------------------------------------------------------

std::string Fernet::decrypt(const std::string& token, std::int64_t ttl_seconds) const {
    auto raw = b64url_decode(token);
    // Minimum: header (25) + at least one ciphertext block (16) + HMAC (32).
    if (raw.size() < kHeaderSize + 16 + kHmacSize) throw FernetError("token too short");
    if (raw[0] != kVersion)                        throw FernetError("unknown token version");

    if (ttl_seconds > 0) {
        std::int64_t ts  = read_be64(raw.data() + 1);
        std::int64_t age = now_seconds() - ts;
        if (age > ttl_seconds)           throw FernetError("token expired");
        if (age < -kMaxClockSkewSeconds) throw FernetError("token timestamp in the future");
    }

    const std::size_t body_len = raw.size() - kHmacSize;
    auto expected = hmac_sha256(signing_key_.data(), signing_key_.size(), raw.data(), body_len);
    if (CRYPTO_memcmp(expected.data(), raw.data() + body_len, kHmacSize) != 0) throw FernetError("HMAC mismatch");

    const std::uint8_t* iv = raw.data() + 1 + 8;
    const std::uint8_t* ct = raw.data() + kHeaderSize;
    const std::size_t ct_len = body_len - kHeaderSize;
    if (ct_len == 0 || (ct_len % 16) != 0) throw FernetError("malformed ciphertext length");

    std::vector<std::uint8_t> pt(ct_len);
    int len_out = 0, total = 0;

    CipherCtx g;
    if (EVP_DecryptInit_ex(g.ctx, EVP_aes_128_cbc(), nullptr, encryption_key_.data(), iv) != 1)
        throw FernetError("EVP_DecryptInit_ex failed");
    if (EVP_DecryptUpdate(g.ctx, pt.data(), &len_out, ct, static_cast<int>(ct_len)) != 1)
        throw FernetError("EVP_DecryptUpdate failed");
    total = len_out;
    if (EVP_DecryptFinal_ex(g.ctx, pt.data() + total, &len_out) != 1)
        throw FernetError("padding/decrypt failed");
    total += len_out;
    pt.resize(static_cast<std::size_t>(total));

    return std::string(pt.begin(), pt.end());
}

}
}

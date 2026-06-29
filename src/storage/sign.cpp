#include "storage/sign.hpp"

#include <openssl/evp.h>

#include <stdexcept>

namespace mirobody { namespace storage {

namespace {

// A std::string over a raw byte array, so the HMAC helpers below can chain their
// own binary output back in as the next key.
template <std::size_t N>
std::string bytes(const std::array<unsigned char, N>& a) {
    return std::string(reinterpret_cast<const char*>(a.data()), a.size());
}

}

//------------------------------------------------------------------------------

std::string hex_encode(const unsigned char* data, std::size_t len) {
    static const char* kHex = "0123456789abcdef";
    std::string out;
    out.reserve(len * 2);
    for (std::size_t i = 0; i < len; ++i) {
        out.push_back(kHex[data[i] >> 4]);
        out.push_back(kHex[data[i] & 0x0F]);
    }
    return out;
}

//------------------------------------------------------------------------------

std::string base64_encode(const void* data, std::size_t length) {
    static const char kAlphabet[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    const auto* p = static_cast<const unsigned char*>(data);
    const std::size_t len = length;
    std::string out;
    out.reserve(((len + 2) / 3) * 4);
    for (std::size_t i = 0; i < len; i += 3) {
        unsigned int n = static_cast<unsigned int>(p[i]) << 16;
        std::size_t take = 1;
        if (i + 1 < len) { n |= static_cast<unsigned int>(p[i + 1]) << 8; take = 2; }
        if (i + 2 < len) { n |= static_cast<unsigned int>(p[i + 2]);      take = 3; }
        out.push_back(kAlphabet[(n >> 18) & 0x3F]);
        out.push_back(kAlphabet[(n >> 12) & 0x3F]);
        out.push_back(take >= 2 ? kAlphabet[(n >> 6) & 0x3F] : '=');
        out.push_back(take >= 3 ? kAlphabet[n & 0x3F]        : '=');
    }
    return out;
}

//------------------------------------------------------------------------------

std::string base64url_encode(const std::string& data) {
    std::string out = base64_encode(data);
    while (!out.empty() && out[out.size() - 1] == '=') out.erase(out.size() - 1);
    for (std::size_t i = 0; i < out.size(); ++i) {
        if (out[i] == '+')      out[i] = '-';
        else if (out[i] == '/') out[i] = '_';
    }
    return out;
}

//------------------------------------------------------------------------------

std::string base64_decode(const std::string& b64) {
    auto val = [](char c) -> int {
        if (c >= 'A' && c <= 'Z') return c - 'A';
        if (c >= 'a' && c <= 'z') return c - 'a' + 26;
        if (c >= '0' && c <= '9') return c - '0' + 52;
        if (c == '+') return 62;
        if (c == '/') return 63;
        return -1;   // '=' padding, whitespace, or other: skipped below
    };
    std::string out;
    out.reserve(b64.size() * 3 / 4);
    int buf = 0, bits = 0;
    for (std::size_t i = 0; i < b64.size(); ++i) {
        const char c = b64[i];
        if (c == '=') break;
        const int v = val(c);
        if (v < 0) continue;   // skip whitespace / stray chars
        buf = (buf << 6) | v;
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            out.push_back(static_cast<char>((buf >> bits) & 0xFF));
        }
    }
    return out;
}

//------------------------------------------------------------------------------

std::string sha256_hex(const std::string& data) {
    unsigned char md[EVP_MAX_MD_SIZE];
    unsigned int md_len = 0;
    if (EVP_Digest(data.data(), data.size(), md, &md_len, EVP_sha256(), nullptr) != 1 ||
        md_len != 32) {
        throw std::runtime_error("SHA-256 failed");
    }
    return hex_encode(md, md_len);
}

//------------------------------------------------------------------------------

std::array<unsigned char, 32> hmac_sha256(const std::string& key, const std::string& data) {
    std::array<unsigned char, 32> out{};
    std::size_t out_len = 0;
    // EVP_Q_mac is the OpenSSL 3.0 one-shot HMAC (same call the Fernet code
    // uses). It allocates and frees the MAC context internally. The data arg is
    // typed const unsigned char*, so cast from the string's char*.
    auto* ok = EVP_Q_mac(nullptr, "HMAC", nullptr, "SHA256", nullptr,
                         key.data(), key.size(),
                         reinterpret_cast<const unsigned char*>(data.data()), data.size(),
                         out.data(), out.size(), &out_len);
    if (ok == nullptr || out_len != out.size()) throw std::runtime_error("HMAC-SHA256 failed");
    return out;
}

//------------------------------------------------------------------------------

std::array<unsigned char, 20> hmac_sha1(const std::string& key, const std::string& data) {
    std::array<unsigned char, 20> out{};
    std::size_t out_len = 0;
    auto* ok = EVP_Q_mac(nullptr, "HMAC", nullptr, "SHA1", nullptr,
                         key.data(), key.size(),
                         reinterpret_cast<const unsigned char*>(data.data()), data.size(),
                         out.data(), out.size(), &out_len);
    if (ok == nullptr || out_len != out.size()) throw std::runtime_error("HMAC-SHA1 failed");
    return out;
}

//------------------------------------------------------------------------------

std::string uri_encode(const std::string& s, bool encode_slash) {
    static const char* kHex = "0123456789ABCDEF";
    std::string out;
    out.reserve(s.size());
    for (char ch : s) {
        const unsigned char c = static_cast<unsigned char>(ch);
        const bool unreserved =
            (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
            (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.' || c == '~';
        if (unreserved || (c == '/' && !encode_slash)) {
            out.push_back(static_cast<char>(c));
        } else {
            out.push_back('%');
            out.push_back(kHex[c >> 4]);
            out.push_back(kHex[c & 0x0F]);
        }
    }
    return out;
}

//------------------------------------------------------------------------------

std::string aws_sigv4_signature(const std::string& secret_key,
                                const std::string& datestamp,
                                const std::string& region,
                                const std::string& service,
                                const std::string& string_to_sign) {
    const std::string seed = "AWS4" + secret_key;
    const auto k_date    = hmac_sha256(seed, datestamp);
    const auto k_region  = hmac_sha256(bytes(k_date), region);
    const auto k_service = hmac_sha256(bytes(k_region), service);
    const auto k_signing = hmac_sha256(bytes(k_service), std::string("aws4_request"));
    const auto sig       = hmac_sha256(bytes(k_signing), string_to_sign);
    return hex_encode(sig.data(), sig.size());
}

//------------------------------------------------------------------------------

std::string oss_signature(const std::string& secret_key, const std::string& string_to_sign) {
    const auto mac = hmac_sha1(secret_key, string_to_sign);
    return base64_encode(mac.data(), mac.size());
}

//------------------------------------------------------------------------------

std::string azure_signature(const std::string& account_key_base64,
                            const std::string& string_to_sign) {
    const std::string key = base64_decode(account_key_base64);
    const auto mac = hmac_sha256(key, string_to_sign);
    return base64_encode(mac.data(), mac.size());
}

//------------------------------------------------------------------------------

namespace {

// Decode the five predefined XML entities. Numeric character references are
// left as-is (the S3 / OSS listings this serves never emit them for keys).
std::string xml_unescape(const std::string& s) {
    static const struct { const char* ent; std::size_t len; char ch; } kEnts[] = {
        {"&amp;",  5, '&'}, {"&lt;",   4, '<'}, {"&gt;", 4, '>'},
        {"&quot;", 6, '"'}, {"&apos;", 6, '\''},
    };
    std::string out;
    out.reserve(s.size());
    std::size_t i = 0;
    while (i < s.size()) {
        bool matched = false;
        if (s[i] == '&') {
            for (std::size_t e = 0; e < sizeof(kEnts) / sizeof(kEnts[0]); ++e) {
                if (s.compare(i, kEnts[e].len, kEnts[e].ent) == 0) {
                    out.push_back(kEnts[e].ch);
                    i += kEnts[e].len;
                    matched = true;
                    break;
                }
            }
        }
        if (!matched) out.push_back(s[i++]);
    }
    return out;
}

}

std::vector<std::string> xml_tag_values(const std::string& xml, const std::string& tag) {
    std::vector<std::string> out;
    const std::string open  = "<" + tag + ">";
    const std::string close = "</" + tag + ">";
    std::size_t pos = 0;
    while ((pos = xml.find(open, pos)) != std::string::npos) {
        const std::size_t b = pos + open.size();
        const std::size_t e = xml.find(close, b);
        if (e == std::string::npos) break;
        out.push_back(xml_unescape(xml.substr(b, e - b)));
        pos = e + close.size();
    }
    return out;
}

std::string xml_tag_value(const std::string& xml, const std::string& tag) {
    const std::vector<std::string> all = xml_tag_values(xml, tag);
    return all.empty() ? std::string() : all[0];
}

}}

#pragma once

// Encoding and request-signing primitives shared by the object-storage
// backends (src/storage/aws_s3.* and src/storage/aliyun_oss.*). Kept in their
// own translation unit so both backends reuse one tested implementation of the
// HMAC / SHA-256 / base64 chain rather than each rolling its own.
//
// The two top-level signers (aws_sigv4_signature / oss_signature) are pure
// functions of their inputs — no clock, no network — which is what makes them
// unit-testable against the providers' published signature test vectors.

#include "compat/cxx11.hpp"

#include <array>
#include <cstddef>
#include <string>
#include <vector>

namespace mirobody { namespace storage {

// Lowercase hex of `len` raw bytes (2 chars per byte).
std::string hex_encode(const unsigned char* data, std::size_t len);

// Standard base64 (RFC 4648, '+' '/' alphabet, '=' padding) of `length` raw
// bytes at `data` -- binary-safe (e.g. a digest), no null-termination assumed.
std::string base64_encode(const void* data, std::size_t length);

// Convenience overload for a std::string (or string literal).
inline std::string base64_encode(const std::string& data) {
    return base64_encode(data.data(), data.size());
}

// URL/filename-safe base64 (RFC 4648 sec.5: '-' '_' alphabet, NO padding) of
// `data` -- safe to embed in object keys and URLs without percent-encoding.
std::string base64url_encode(const std::string& data);

// Decode standard base64 ('+' '/' alphabet) to raw bytes. Tolerates missing
// padding and skips embedded whitespace/newlines; stops at the first '='. Used
// to turn an Azure storage account key (base64 text) back into the HMAC key.
std::string base64_decode(const std::string& b64);

// Lowercase-hex SHA-256 of `data`. sha256_hex("") == e3b0c442...b855 (the
// empty-string digest AWS uses for empty payloads).
std::string sha256_hex(const std::string& data);

// Raw HMAC outputs (binary, not hex). Keys and data are arbitrary byte ranges.
std::array<unsigned char, 32> hmac_sha256(const std::string& key, const std::string& data);
std::array<unsigned char, 20> hmac_sha1  (const std::string& key, const std::string& data);

// Percent-encode per RFC 3986: unreserved chars (A-Z a-z 0-9 - _ . ~) pass
// through, everything else becomes %XX with UPPER-case hex. When
// `encode_slash` is false, '/' is also passed through unescaped — used for the
// path component of a canonical URI, where segment separators must survive.
std::string uri_encode(const std::string& s, bool encode_slash);

// AWS Signature Version 4: derive the date/region/service signing key from
// `secret_key` and return the lowercase-hex signature of `string_to_sign`.
//   kDate    = HMAC("AWS4"+secret, datestamp)      datestamp = YYYYMMDD (UTC)
//   kRegion  = HMAC(kDate, region)
//   kService = HMAC(kRegion, service)              service   = "s3"
//   kSigning = HMAC(kService, "aws4_request")
//   signature= hex(HMAC(kSigning, string_to_sign))
std::string aws_sigv4_signature(const std::string& secret_key,
                                const std::string& datestamp,
                                const std::string& region,
                                const std::string& service,
                                const std::string& string_to_sign);

// Aliyun OSS classic (header/URL) signature: base64(HMAC-SHA1(secret_key,
// string_to_sign)). The caller assembles `string_to_sign` per the OSS spec
// (VERB\nContent-MD5\nContent-Type\nDate\nCanonicalizedResource, with any
// CanonicalizedOSSHeaders folded in before the resource).
std::string oss_signature(const std::string& secret_key,
                          const std::string& string_to_sign);

// Azure Blob signature: base64(HMAC-SHA256(base64-decode(account_key),
// string_to_sign)). The same primitive backs both the Shared Key
// authorization header and a service SAS — the caller assembles the
// version-specific string_to_sign per the Azure spec and hands it here.
// `account_key_base64` is the storage account key as configured (base64 text).
std::string azure_signature(const std::string& account_key_base64,
                            const std::string& string_to_sign);

// Values of every non-nested <tag>...</tag> in `xml`, in document order, with
// the five predefined XML entities decoded. A flat, attribute-less scan --
// sufficient for the S3 / OSS object-listing responses it exists for.
std::vector<std::string> xml_tag_values(const std::string& xml, const std::string& tag);

// First occurrence, or "" when absent / empty.
std::string xml_tag_value(const std::string& xml, const std::string& tag);

}}

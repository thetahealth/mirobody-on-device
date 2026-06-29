#include "storage/aws_s3.hpp"

#include "storage/sign.hpp"

#include <ctime>
#include <map>
#include <string>

namespace mirobody { namespace storage {

namespace {

constexpr const char* kService   = "s3";
constexpr const char* kAlgorithm = "AWS4-HMAC-SHA256";
constexpr const char* kScopeTail = "aws4_request";

//------------------------------------------------------------------------------

// UTC timestamps in the two formats SigV4 needs: the full ISO basic stamp for
// the X-Amz-Date header / string-to-sign, and the bare day for the credential
// scope and signing-key derivation.
struct TimeStamp {
    std::string amzdate;    // YYYYMMDDTHHMMSSZ
    std::string datestamp;  // YYYYMMDD
};

TimeStamp utc_now() {
    std::time_t t = std::time(nullptr);
    std::tm tm{};
#ifdef _WIN32
    gmtime_s(&tm, &t);
#else
    gmtime_r(&t, &tm);
#endif
    char amz[20];
    char day[16];
    std::strftime(amz, sizeof(amz), "%Y%m%dT%H%M%SZ", &tm);
    std::strftime(day, sizeof(day), "%Y%m%d", &tm);
    return TimeStamp{amz, day};
}

//------------------------------------------------------------------------------

// Strip a leading "http://" / "https://" and any trailing '/' from a host.
std::string host_only(const std::string& endpoint) {
    std::string h = endpoint;
    const auto scheme = h.find("://");
    if (scheme != std::string::npos) h = h.substr(scheme + 3);
    while (!h.empty() && h.back() == '/') h.pop_back();
    return h;
}

//------------------------------------------------------------------------------

std::string trim_slashes(const std::string& s) {
    std::size_t b = 0, e = s.size();
    while (b < e && s[b] == '/') ++b;
    while (e > b && s[e - 1] == '/') --e;
    return std::string(s.substr(b, e - b));
}

}

//------------------------------------------------------------------------------

AwsS3::AwsS3(S3Config config) : config_(std::move(config)) {
    if (!config_.configured() || config_.region.empty()) {
        throw StorageError("AwsS3: access_key, secret_key, bucket and region are all required");
    }
    if (config_.endpoint.empty()) {
        // Real AWS: virtual-hosted-style. The bucket lives in the host, so the
        // request path is just the (prefixed) object key.
        host_ = config_.bucket + ".s3." + config_.region + ".amazonaws.com";
        path_style_ = false;
    } else {
        // S3-compatible store (MinIO, Cloudflare R2, …): path-style against the
        // given endpoint host, with the bucket as the first path segment.
        host_ = host_only(config_.endpoint);
        path_style_ = true;
    }
}

//------------------------------------------------------------------------------

std::string AwsS3::full_key(const std::string& key) const {
    const std::string k = trim_slashes(key);
    if (config_.prefix.empty()) return k;
    const std::string p = trim_slashes(config_.prefix) + "/";
    // Idempotent: a key already carrying the prefix (any API-returned key)
    // passes through unchanged.
    if (k.compare(0, p.size(), p) == 0) return k;
    return p + k;
}

//------------------------------------------------------------------------------

std::string AwsS3::canonical_uri(const std::string& fk) const {
    // Encode each path segment but keep the separators (encode_slash=false).
    std::string path = "/" + uri_encode(fk, /*encode_slash=*/false);
    if (path_style_) path = "/" + config_.bucket + path;
    return path;
}

//------------------------------------------------------------------------------

std::string AwsS3::public_url(const std::string& key) const {
    const std::string fk = full_key(key);
    if (!config_.cdn.empty()) {
        return "https://" + host_only(config_.cdn) + "/" + fk;
    }
    return "https://" + host_ + canonical_uri(fk);
}

//------------------------------------------------------------------------------

std::string AwsS3::put_object(const std::string& key,
                              const std::string& data,
                              const std::string& content_type) {
    const std::string fk  = full_key(key);
    const std::string uri = canonical_uri(fk);
    const TimeStamp ts = utc_now();
    const std::string payload_hash = sha256_hex(data);
    const std::string ct(content_type);

    // Canonical headers, sorted by lower-cased name. content-type sorts before
    // host, which sorts before the x-amz-* pair.
    const std::string canonical_headers =
        "content-type:" + ct + "\n" +
        "host:" + host_ + "\n" +
        "x-amz-content-sha256:" + payload_hash + "\n" +
        "x-amz-date:" + ts.amzdate + "\n";
    const std::string signed_headers = "content-type;host;x-amz-content-sha256;x-amz-date";

    const std::string canonical_request =
        "PUT\n" + uri + "\n" + std::string() + "\n" +
        canonical_headers + "\n" + signed_headers + "\n" + payload_hash;

    const std::string scope = ts.datestamp + "/" + config_.region + "/" + kService + "/" + kScopeTail;
    const std::string string_to_sign =
        std::string(kAlgorithm) + "\n" + ts.amzdate + "\n" + scope + "\n" + sha256_hex(canonical_request);
    const std::string signature =
        aws_sigv4_signature(config_.secret_key, ts.datestamp, config_.region, kService, string_to_sign);

    const std::string authorization =
        std::string(kAlgorithm) + " Credential=" + config_.access_key + "/" + scope +
        ", SignedHeaders=" + signed_headers + ", Signature=" + signature;

    client::HttpRequest req;
    req.url  = "https://" + host_ + uri;
    req.body = std::string(data);
    req.headers = {
        "Content-Type: " + ct,
        "x-amz-content-sha256: " + payload_hash,
        "x-amz-date: " + ts.amzdate,
        "Authorization: " + authorization,
    };
    client::HttpResponse resp = http_.request("PUT", req);
    if (resp.status < 200 || resp.status >= 300) {
        throw StorageError("AwsS3 put_object failed (HTTP " + std::to_string(resp.status) + "): " + resp.body);
    }
    return key;
}

//------------------------------------------------------------------------------

std::string AwsS3::get_object(const std::string& key) {
    const std::string fk  = full_key(key);
    const std::string uri = canonical_uri(fk);
    const TimeStamp ts = utc_now();
    const std::string payload_hash = sha256_hex("");   // empty body

    const std::string canonical_headers =
        "host:" + host_ + "\n" +
        "x-amz-content-sha256:" + payload_hash + "\n" +
        "x-amz-date:" + ts.amzdate + "\n";
    const std::string signed_headers = "host;x-amz-content-sha256;x-amz-date";

    const std::string canonical_request =
        "GET\n" + uri + "\n" + std::string() + "\n" +
        canonical_headers + "\n" + signed_headers + "\n" + payload_hash;

    const std::string scope = ts.datestamp + "/" + config_.region + "/" + kService + "/" + kScopeTail;
    const std::string string_to_sign =
        std::string(kAlgorithm) + "\n" + ts.amzdate + "\n" + scope + "\n" + sha256_hex(canonical_request);
    const std::string signature =
        aws_sigv4_signature(config_.secret_key, ts.datestamp, config_.region, kService, string_to_sign);

    const std::string authorization =
        std::string(kAlgorithm) + " Credential=" + config_.access_key + "/" + scope +
        ", SignedHeaders=" + signed_headers + ", Signature=" + signature;

    client::HttpRequest req;
    req.url = "https://" + host_ + uri;
    req.headers = {
        "x-amz-content-sha256: " + payload_hash,
        "x-amz-date: " + ts.amzdate,
        "Authorization: " + authorization,
    };
    client::HttpResponse resp = http_.request("GET", req);
    if (resp.status < 200 || resp.status >= 300) {
        throw StorageError("AwsS3 get_object failed (HTTP " + std::to_string(resp.status) + "): " + resp.body);
    }
    return resp.body;
}

//------------------------------------------------------------------------------

std::vector<std::string> AwsS3::list_objects(const std::string& prefix,
                                             std::size_t max_keys) {
    // The prefix keeps its trailing slash (meaningful for a prefix scan, so
    // full_key's slash-trimming doesn't apply); only leading slashes go. The
    // configured bucket prefix is applied like full_key does -- only when
    // the caller's prefix doesn't already carry it.
    std::string p(prefix);
    while (!p.empty() && p[0] == '/') p.erase(0, 1);
    const std::string cfg_prefix = config_.prefix.empty()
        ? std::string() : trim_slashes(config_.prefix) + "/";
    const std::string fp =
        (!cfg_prefix.empty() && p.compare(0, cfg_prefix.size(), cfg_prefix) == 0)
            ? p : cfg_prefix + p;

    // Bucket-level request: virtual-hosted addresses the bucket via the host,
    // path-style via the first path segment.
    const std::string uri = path_style_ ? "/" + config_.bucket : std::string("/");

    std::vector<std::string> out;
    std::string token;   // ListObjectsV2 continuation token, page 2 onward
    while (out.size() < max_keys) {
        const std::size_t remaining = max_keys - out.size();

        // Query parameters; std::map keeps them in the byte-sorted order the
        // SigV4 canonical query string requires.
        std::map<std::string, std::string> q;
        q["list-type"] = "2";
        q["max-keys"]  = std::to_string(remaining < 1000 ? remaining : 1000);
        if (!fp.empty())    q["prefix"] = fp;
        if (!token.empty()) q["continuation-token"] = token;

        std::string canonical_query;
        for (const auto& kv : q) {
            if (!canonical_query.empty()) canonical_query += '&';
            canonical_query += uri_encode(kv.first, true) + "=" + uri_encode(kv.second, true);
        }

        const TimeStamp ts = utc_now();
        const std::string payload_hash = sha256_hex("");   // empty body

        const std::string canonical_headers =
            "host:" + host_ + "\n" +
            "x-amz-content-sha256:" + payload_hash + "\n" +
            "x-amz-date:" + ts.amzdate + "\n";
        const std::string signed_headers = "host;x-amz-content-sha256;x-amz-date";

        const std::string canonical_request =
            "GET\n" + uri + "\n" + canonical_query + "\n" +
            canonical_headers + "\n" + signed_headers + "\n" + payload_hash;

        const std::string scope = ts.datestamp + "/" + config_.region + "/" + kService + "/" + kScopeTail;
        const std::string string_to_sign =
            std::string(kAlgorithm) + "\n" + ts.amzdate + "\n" + scope + "\n" + sha256_hex(canonical_request);
        const std::string signature =
            aws_sigv4_signature(config_.secret_key, ts.datestamp, config_.region, kService, string_to_sign);

        const std::string authorization =
            std::string(kAlgorithm) + " Credential=" + config_.access_key + "/" + scope +
            ", SignedHeaders=" + signed_headers + ", Signature=" + signature;

        client::HttpRequest req;
        req.url = "https://" + host_ + uri + "?" + canonical_query;
        req.headers = {
            "x-amz-content-sha256: " + payload_hash,
            "x-amz-date: " + ts.amzdate,
            "Authorization: " + authorization,
        };
        client::HttpResponse resp = http_.request("GET", req);
        if (resp.status < 200 || resp.status >= 300) {
            throw StorageError("AwsS3 list_objects failed (HTTP " + std::to_string(resp.status) + "): " + resp.body);
        }

        // <Contents><Key>...</Key>...</Contents> per object; no delimiter is
        // sent, so <Key> only occurs inside <Contents>. The service returns
        // full keys (configured prefix included); strip the prefix so callers
        // get prefix-less keys -- the form every API method returns and accepts
        // (full_key re-applies the prefix internally at access time).
        const std::vector<std::string> keys = xml_tag_values(resp.body, "Key");
        for (std::size_t i = 0; i < keys.size() && out.size() < max_keys; ++i) {
            const std::string& k = keys[i];
            out.push_back((!cfg_prefix.empty() && k.compare(0, cfg_prefix.size(), cfg_prefix) == 0)
                          ? k.substr(cfg_prefix.size()) : k);
        }

        if (xml_tag_value(resp.body, "IsTruncated") != "true") break;
        token = xml_tag_value(resp.body, "NextContinuationToken");
        if (token.empty()) break;   // defensive: truncated but no token
    }
    return out;
}

//------------------------------------------------------------------------------

void AwsS3::delete_object(const std::string& key) {
    const std::string fk  = full_key(key);
    const std::string uri = canonical_uri(fk);
    const TimeStamp ts = utc_now();
    const std::string payload_hash = sha256_hex("");

    const std::string canonical_headers =
        "host:" + host_ + "\n" +
        "x-amz-content-sha256:" + payload_hash + "\n" +
        "x-amz-date:" + ts.amzdate + "\n";
    const std::string signed_headers = "host;x-amz-content-sha256;x-amz-date";

    const std::string canonical_request =
        "DELETE\n" + uri + "\n" + std::string() + "\n" +
        canonical_headers + "\n" + signed_headers + "\n" + payload_hash;

    const std::string scope = ts.datestamp + "/" + config_.region + "/" + kService + "/" + kScopeTail;
    const std::string string_to_sign =
        std::string(kAlgorithm) + "\n" + ts.amzdate + "\n" + scope + "\n" + sha256_hex(canonical_request);
    const std::string signature =
        aws_sigv4_signature(config_.secret_key, ts.datestamp, config_.region, kService, string_to_sign);

    const std::string authorization =
        std::string(kAlgorithm) + " Credential=" + config_.access_key + "/" + scope +
        ", SignedHeaders=" + signed_headers + ", Signature=" + signature;

    client::HttpRequest req;
    req.url = "https://" + host_ + uri;
    req.headers = {
        "x-amz-content-sha256: " + payload_hash,
        "x-amz-date: " + ts.amzdate,
        "Authorization: " + authorization,
    };
    client::HttpResponse resp = http_.request("DELETE", req);
    // S3 returns 204 for a successful delete and also 204 for a missing key
    // (delete is idempotent). Treat 404 as success too for path-style stores
    // that surface it.
    if (resp.status != 204 && resp.status != 200 && resp.status != 404) {
        throw StorageError("AwsS3 delete_object failed (HTTP " + std::to_string(resp.status) + "): " + resp.body);
    }
}

//------------------------------------------------------------------------------

std::string AwsS3::presigned_url(const std::string& key, int expires_seconds) const {
    const std::string fk  = full_key(key);
    const std::string uri = canonical_uri(fk);
    const TimeStamp ts = utc_now();

    const std::string scope = ts.datestamp + "/" + config_.region + "/" + kService + "/" + kScopeTail;
    const std::string credential = config_.access_key + "/" + scope;

    // Query parameters that carry the signature inputs. std::map keeps them in
    // the byte-sorted order SigV4 requires for the canonical query string.
    std::map<std::string, std::string> q;
    q["X-Amz-Algorithm"]     = kAlgorithm;
    q["X-Amz-Credential"]    = credential;
    q["X-Amz-Date"]          = ts.amzdate;
    q["X-Amz-Expires"]       = std::to_string(expires_seconds);
    q["X-Amz-SignedHeaders"] = "host";

    std::string canonical_query;
    for (const auto& kv : q) {
        if (!canonical_query.empty()) canonical_query += '&';
        canonical_query += uri_encode(kv.first, true) + "=" + uri_encode(kv.second, true);
    }

    const std::string canonical_headers = "host:" + host_ + "\n";
    const std::string signed_headers = "host";

    const std::string canonical_request =
        "GET\n" + uri + "\n" + canonical_query + "\n" +
        canonical_headers + "\n" + signed_headers + "\nUNSIGNED-PAYLOAD";

    const std::string string_to_sign =
        std::string(kAlgorithm) + "\n" + ts.amzdate + "\n" + scope + "\n" + sha256_hex(canonical_request);
    const std::string signature =
        aws_sigv4_signature(config_.secret_key, ts.datestamp, config_.region, kService, string_to_sign);

    return "https://" + host_ + uri + "?" + canonical_query + "&X-Amz-Signature=" + signature;
}

//------------------------------------------------------------------------------

std::unique_ptr<Storage> S3Config::open() const {
    return std::unique_ptr<Storage>(new AwsS3(*this));
}

}}

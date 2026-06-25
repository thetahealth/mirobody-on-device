#include "storage/azure_blob.hpp"

#include "storage/sign.hpp"

#include <algorithm>
#include <cstdio>
#include <ctime>
#include <map>
#include <string>

namespace mirobody { namespace storage {

namespace {

// The Storage Services REST API version this backend signs and sends. The
// Shared Key string-to-sign layout and the service-SAS field set are
// version-specific, so the signed `x-ms-version` and the SAS `sv` must match
// this exact value.
const char* const kApiVersion = "2021-08-06";

//------------------------------------------------------------------------------

// RFC 1123 GMT, e.g. "Thu, 17 Nov 2005 18:49:58 GMT" -- from fixed English
// tables (never the locale), since the value is signed.
std::string rfc1123_gmt(std::time_t t) {
    static const char* kDays[]   = {"Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat"};
    static const char* kMonths[] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun",
                                     "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};
    std::tm tm{};
#ifdef _WIN32
    gmtime_s(&tm, &t);
#else
    gmtime_r(&t, &tm);
#endif
    char buf[40];
    std::snprintf(buf, sizeof(buf), "%s, %02d %s %04d %02d:%02d:%02d GMT",
                  kDays[tm.tm_wday], tm.tm_mday, kMonths[tm.tm_mon], tm.tm_year + 1900,
                  tm.tm_hour, tm.tm_min, tm.tm_sec);
    return std::string(buf);
}

// ISO 8601 UTC, e.g. "2026-01-02T03:04:05Z" -- the SAS signedExpiry format.
std::string iso8601_utc(std::time_t t) {
    std::tm tm{};
#ifdef _WIN32
    gmtime_s(&tm, &t);
#else
    gmtime_r(&t, &tm);
#endif
    char buf[24];
    std::strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%SZ", &tm);
    return std::string(buf);
}

std::string trim_slashes(const std::string& s) {
    std::size_t b = 0, e = s.size();
    while (b < e && s[b] == '/') ++b;
    while (e > b && s[e - 1] == '/') --e;
    return std::string(s.substr(b, e - b));
}

std::string host_only(const std::string& s) {
    std::string h = s;
    const auto scheme = h.find("://");
    if (scheme != std::string::npos) h = h.substr(scheme + 3);
    while (!h.empty() && h.back() == '/') h.pop_back();
    return h;
}

}  // namespace

//------------------------------------------------------------------------------

AzureBlob::AzureBlob(AzureBlobConfig config) : config_(std::move(config)) {
    if (!config_.configured()) {
        throw StorageError("AzureBlob: account, key and container are all required");
    }
    if (config_.endpoint_suffix.empty()) config_.endpoint_suffix = "core.windows.net";
    host_ = config_.account + ".blob." + host_only(config_.endpoint_suffix);
}

//------------------------------------------------------------------------------

std::string AzureBlob::full_key(const std::string& key) const {
    const std::string k = trim_slashes(key);
    if (config_.prefix.empty()) return k;
    const std::string p = trim_slashes(config_.prefix) + "/";
    if (k.compare(0, p.size(), p) == 0) return k;   // idempotent
    return p + k;
}

//------------------------------------------------------------------------------

std::string AzureBlob::canonical_resource(const std::string& fk,
                                          const std::vector<std::string>& query_lines) const {
    std::string r = "/" + config_.account + "/" + config_.container;
    if (!fk.empty()) r += "/" + fk;
    for (std::size_t i = 0; i < query_lines.size(); ++i) {
        r += "\n";
        r += query_lines[i];
    }
    return r;
}

std::string AzureBlob::object_url(const std::string& fk) const {
    return "https://" + host_ + "/" + config_.container + "/" + uri_encode(fk, /*encode_slash=*/false);
}

//------------------------------------------------------------------------------

std::string AzureBlob::authorization(const std::string& verb, const std::string& content_type,
                                     std::size_t content_length,
                                     const std::vector<std::string>& canonical_headers,
                                     const std::string& canonical_resource) const {
    // Content-Length is an empty string when zero (2014-02-14+ rule).
    const std::string clen = content_length > 0 ? std::to_string(content_length) : std::string();

    std::string headers;   // each "x-ms-name:value", joined with a trailing '\n'
    for (std::size_t i = 0; i < canonical_headers.size(); ++i) {
        headers += canonical_headers[i];
        headers += "\n";
    }

    // The Shared Key string-to-sign: 12 fixed request-header slots, then the
    // canonicalized x-ms-* headers, then the canonicalized resource.
    const std::string sts =
        verb + "\n"            // VERB
        "\n"                   // Content-Encoding
        "\n"                   // Content-Language
        + clen + "\n"          // Content-Length
        + "\n"                 // Content-MD5
        + content_type + "\n"  // Content-Type
        + "\n"                 // Date (empty: x-ms-date is used instead)
        "\n"                   // If-Modified-Since
        "\n"                   // If-Match
        "\n"                   // If-None-Match
        "\n"                   // If-Unmodified-Since
        "\n"                   // Range
        + headers              // CanonicalizedHeaders (each already ends with '\n')
        + canonical_resource;  // CanonicalizedResource (no trailing '\n')

    return "SharedKey " + config_.account + ":" + azure_signature(config_.key, sts);
}

//------------------------------------------------------------------------------

std::string AzureBlob::put_object(const std::string& key,
                                  const std::string& data,
                                  const std::string& content_type) {
    const std::string fk = full_key(key);
    const std::string date = rfc1123_gmt(std::time(nullptr));
    const std::string ct(content_type);

    // Canonical x-ms-* headers, lowercased and sorted: blob-type < date < version.
    const std::vector<std::string> ch = {
        "x-ms-blob-type:BlockBlob",
        "x-ms-date:" + date,
        std::string("x-ms-version:") + kApiVersion,
    };
    const std::string auth = authorization("PUT", ct, data.size(), ch, canonical_resource(fk));

    client::HttpRequest req;
    req.url  = object_url(fk);
    req.body = std::string(data);
    req.headers = {
        "x-ms-blob-type: BlockBlob",
        "x-ms-date: " + date,
        std::string("x-ms-version: ") + kApiVersion,
        "Content-Type: " + ct,
        "Authorization: " + auth,
    };
    client::HttpResponse resp = http_.request("PUT", req);
    if (resp.status < 200 || resp.status >= 300) {
        throw StorageError("AzureBlob put_object failed (HTTP " + std::to_string(resp.status) + "): " + resp.body);
    }
    return key;
}

//------------------------------------------------------------------------------

std::string AzureBlob::get_object(const std::string& key) {
    const std::string fk = full_key(key);
    const std::string date = rfc1123_gmt(std::time(nullptr));

    const std::vector<std::string> ch = {
        "x-ms-date:" + date,
        std::string("x-ms-version:") + kApiVersion,
    };
    const std::string auth = authorization("GET", "", 0, ch, canonical_resource(fk));

    client::HttpRequest req;
    req.url = object_url(fk);
    req.headers = {
        "x-ms-date: " + date,
        std::string("x-ms-version: ") + kApiVersion,
        "Authorization: " + auth,
    };
    client::HttpResponse resp = http_.request("GET", req);
    if (resp.status < 200 || resp.status >= 300) {
        throw StorageError("AzureBlob get_object failed (HTTP " + std::to_string(resp.status) + "): " + resp.body);
    }
    return resp.body;
}

//------------------------------------------------------------------------------

void AzureBlob::delete_object(const std::string& key) {
    const std::string fk = full_key(key);
    const std::string date = rfc1123_gmt(std::time(nullptr));

    const std::vector<std::string> ch = {
        "x-ms-date:" + date,
        std::string("x-ms-version:") + kApiVersion,
    };
    const std::string auth = authorization("DELETE", "", 0, ch, canonical_resource(fk));

    client::HttpRequest req;
    req.url = object_url(fk);
    req.headers = {
        "x-ms-date: " + date,
        std::string("x-ms-version: ") + kApiVersion,
        "Authorization: " + auth,
    };
    client::HttpResponse resp = http_.request("DELETE", req);
    // 202 Accepted on success; a missing blob (404) is not an error (idempotent).
    if (resp.status != 202 && resp.status != 200 && resp.status != 404) {
        throw StorageError("AzureBlob delete_object failed (HTTP " + std::to_string(resp.status) + "): " + resp.body);
    }
}

//------------------------------------------------------------------------------

std::vector<std::string> AzureBlob::list_objects(const std::string& prefix,
                                                 std::size_t max_keys) {
    std::string p(prefix);
    while (!p.empty() && p[0] == '/') p.erase(0, 1);
    const std::string cfg_prefix = config_.prefix.empty()
        ? std::string() : trim_slashes(config_.prefix) + "/";
    const std::string fp =
        (!cfg_prefix.empty() && p.compare(0, cfg_prefix.size(), cfg_prefix) == 0)
            ? p : cfg_prefix + p;

    std::vector<std::string> out;
    std::string marker;   // continuation token, page 2 onward
    while (out.size() < max_keys) {
        const std::size_t remaining = max_keys - out.size();
        const std::string date = rfc1123_gmt(std::time(nullptr));

        // List Blobs: GET <container>?restype=container&comp=list&... Every query
        // parameter rides in the canonicalized resource, lowercased and sorted by
        // name (a std::map gives that order); the URL repeats them, value-encoded.
        std::map<std::string, std::string> params;
        params["comp"]       = "list";
        params["restype"]    = "container";
        params["maxresults"] = std::to_string(remaining < 5000 ? remaining : 5000);
        if (!fp.empty())     params["prefix"] = fp;
        if (!marker.empty()) params["marker"] = marker;

        std::vector<std::string> query_lines;
        std::string query;
        for (std::map<std::string, std::string>::const_iterator it = params.begin();
             it != params.end(); ++it) {
            query_lines.push_back(it->first + ":" + it->second);
            if (!query.empty()) query += "&";
            query += it->first + "=" + uri_encode(it->second, /*encode_slash=*/true);
        }

        const std::vector<std::string> ch = {
            "x-ms-date:" + date,
            std::string("x-ms-version:") + kApiVersion,
        };
        const std::string auth = authorization("GET", "", 0, ch, canonical_resource("", query_lines));

        client::HttpRequest req;
        req.url = "https://" + host_ + "/" + config_.container + "?" + query;
        req.headers = {
            "x-ms-date: " + date,
            std::string("x-ms-version: ") + kApiVersion,
            "Authorization: " + auth,
        };
        client::HttpResponse resp = http_.request("GET", req);
        if (resp.status < 200 || resp.status >= 300) {
            throw StorageError("AzureBlob list_objects failed (HTTP " + std::to_string(resp.status) + "): " + resp.body);
        }

        // <Blobs><Blob><Name>key</Name>...</Blob></Blobs>. With no delimiter sent
        // there are no <BlobPrefix> entries, so <Name> is always a blob name.
        // Strip the configured prefix so callers get prefix-less keys.
        const std::vector<std::string> names = xml_tag_values(resp.body, "Name");
        for (std::size_t i = 0; i < names.size() && out.size() < max_keys; ++i) {
            const std::string& k = names[i];
            out.push_back((!cfg_prefix.empty() && k.compare(0, cfg_prefix.size(), cfg_prefix) == 0)
                          ? k.substr(cfg_prefix.size()) : k);
        }

        marker = xml_tag_value(resp.body, "NextMarker");
        if (marker.empty()) break;   // last page
    }
    return out;
}

//------------------------------------------------------------------------------

std::string AzureBlob::presigned_url(const std::string& key, int expires_seconds) const {
    const std::string fk = full_key(key);
    const std::string expiry = iso8601_utc(std::time(nullptr) + expires_seconds);

    // Service SAS for a blob, read-only. The signed string's field set is fixed
    // for the signed version (sv); these slots match kApiVersion (2020-12-06+,
    // which adds signedEncryptionScope). Empty slots (start, identifier, ip,
    // snapshot, scope, response-override headers) are present as blank lines.
    const std::string canonical = "/blob/" + config_.account + "/" + config_.container + "/" + fk;
    const std::string sts =
        std::string("r") + "\n"        // signedPermissions
        + "\n"                          // signedStart
        + expiry + "\n"                 // signedExpiry
        + canonical + "\n"              // canonicalizedResource
        + "\n"                          // signedIdentifier
        + "\n"                          // signedIP
        + "https" + "\n"                // signedProtocol
        + kApiVersion + "\n"            // signedVersion
        + "b" + "\n"                    // signedResource (blob)
        + "\n"                          // signedSnapshotTime
        + "\n"                          // signedEncryptionScope
        + "\n"                          // rscc (Cache-Control)
        + "\n"                          // rscd (Content-Disposition)
        + "\n"                          // rsce (Content-Encoding)
        + "\n"                          // rscl (Content-Language)
        + "";                           // rsct (Content-Type) -- last, no '\n'
    const std::string sig = azure_signature(config_.key, sts);

    return object_url(fk) +
           "?sv=" + std::string(kApiVersion) +
           "&sr=b&sp=r&spr=https" +
           "&se=" + uri_encode(expiry, /*encode_slash=*/true) +
           "&sig=" + uri_encode(sig, /*encode_slash=*/true);
}

//------------------------------------------------------------------------------

std::string AzureBlob::public_url(const std::string& key) const {
    const std::string fk = full_key(key);
    if (!config_.cdn.empty()) {
        return "https://" + host_only(config_.cdn) + "/" + uri_encode(fk, false);
    }
    return object_url(fk);
}

//------------------------------------------------------------------------------

std::unique_ptr<Storage> AzureBlobConfig::open() const {
    return std::unique_ptr<Storage>(new AzureBlob(*this));
}

}}

#include "storage/aliyun_oss.hpp"

#include "storage/sign.hpp"

#include <cstdio>
#include <ctime>
#include <string>

namespace mirobody { namespace storage {

namespace {

//------------------------------------------------------------------------------

// RFC 1123 date in GMT, e.g. "Thu, 17 Nov 2005 18:49:58 GMT". Built from fixed
// English day/month tables rather than strftime("%a"/"%b") so the output never
// depends on the process locale — the value goes straight into the signature,
// so a localized weekday name would silently break every request.
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

//------------------------------------------------------------------------------

std::string trim_slashes(const std::string& s) {
    std::size_t b = 0, e = s.size();
    while (b < e && s[b] == '/') ++b;
    while (e > b && s[e - 1] == '/') --e;
    return std::string(s.substr(b, e - b));
}

// Strip scheme and trailing slashes from a host-ish config value.
std::string host_only(const std::string& s) {
    std::string h = s;
    const auto scheme = h.find("://");
    if (scheme != std::string::npos) h = h.substr(scheme + 3);
    while (!h.empty() && h.back() == '/') h.pop_back();
    return h;
}

}

//------------------------------------------------------------------------------

AliyunOss::AliyunOss(OssConfig config) : config_(std::move(config)) {
    if (!config_.configured()) {
        throw StorageError("AliyunOss: access_key_id, secret_access_key, endpoint and bucket are all required");
    }
    host_ = config_.bucket + "." + host_only(config_.endpoint);
}

//------------------------------------------------------------------------------

std::string AliyunOss::full_key(const std::string& key) const {
    const std::string k = trim_slashes(key);
    if (config_.prefix.empty()) return k;
    const std::string p = trim_slashes(config_.prefix) + "/";
    // Idempotent: a key already carrying the prefix (any API-returned key)
    // passes through unchanged.
    if (k.compare(0, p.size(), p) == 0) return k;
    return p + k;
}

//------------------------------------------------------------------------------

std::string AliyunOss::canonical_resource(const std::string& fk) const {
    // The signed resource uses the raw (un-encoded) object name.
    return "/" + config_.bucket + "/" + fk;
}

//------------------------------------------------------------------------------

std::string AliyunOss::object_url(const std::string& fk) const {
    return "https://" + host_ + "/" + uri_encode(fk, /*encode_slash=*/false);
}

//------------------------------------------------------------------------------

std::string AliyunOss::public_url(const std::string& key) const {
    const std::string fk = full_key(key);
    if (!config_.cdn.empty()) {
        return "https://" + host_only(config_.cdn) + "/" + uri_encode(fk, false);
    }
    return object_url(fk);
}

//------------------------------------------------------------------------------

std::string AliyunOss::put_object(const std::string& key,
                                  const std::string& data,
                                  const std::string& content_type) {
    const std::string fk = full_key(key);
    const std::string date = rfc1123_gmt(std::time(nullptr));
    const std::string ct(content_type);

    // StringToSign = VERB\nContent-MD5\nContent-Type\nDate\nCanonicalizedResource
    // (no x-oss-* headers, so CanonicalizedOSSHeaders is empty). Content-MD5 is
    // left blank — OSS treats it as optional.
    const std::string string_to_sign =
        "PUT\n\n" + ct + "\n" + date + "\n" + canonical_resource(fk);
    const std::string signature = oss_signature(config_.secret_access_key, string_to_sign);

    client::HttpRequest req;
    req.url  = object_url(fk);
    req.body = std::string(data);
    req.headers = {
        "Content-Type: " + ct,
        "Date: " + date,
        "Authorization: OSS " + config_.access_key_id + ":" + signature,
    };
    client::HttpResponse resp = http_.request("PUT", req);
    if (resp.status < 200 || resp.status >= 300) {
        throw StorageError("AliyunOss put_object failed (HTTP " + std::to_string(resp.status) + "): " + resp.body);
    }
    return key;
}

//------------------------------------------------------------------------------

std::string AliyunOss::append_to_object(const std::string& key,
                                        const std::string& data,
                                        const std::string& content_type) {
    const std::string fk = full_key(key);
    const std::string ct(content_type);

    // AppendObject: POST ?append&position=N, where N must equal the current
    // length. Start optimistically at 0 (a new object, the common case for
    // logs); when the object already has content OSS answers 409
    // PositionNotEqualToLength and names the right position in
    // x-oss-next-append-position, so retry there -- no HEAD round-trip. The
    // retry bound covers concurrent appenders moving the position.
    std::string position = "0";
    for (int attempt = 0; attempt < 4; ++attempt) {
        const std::string sub  = "?append&position=" + position;
        const std::string date = rfc1123_gmt(std::time(nullptr));

        // Both "append" and "position" are subresources, so they ride in the
        // signed CanonicalizedResource (sorted: append < position).
        const std::string string_to_sign =
            "POST\n\n" + ct + "\n" + date + "\n" + canonical_resource(fk) + sub;
        const std::string signature = oss_signature(config_.secret_access_key, string_to_sign);

        client::HttpRequest req;
        req.url  = object_url(fk) + sub;
        req.body = std::string(data);
        req.headers = {
            "Content-Type: " + ct,
            "Date: " + date,
            "Authorization: OSS " + config_.access_key_id + ":" + signature,
        };
        client::HttpResponse resp = http_.request("POST", req);
        // Return the prefix-less key (matching put_object and the base append's
        // read-modify-write fallback below), not the full key.
        if (resp.status >= 200 && resp.status < 300) return std::string(key.data(), key.size());

        if (resp.status == 409 &&
            resp.body.find("ObjectNotAppendable") != std::string::npos) {
            // Created by put_object (type Normal): native append is
            // impossible; concatenate via the base read-modify-write.
            return Storage::append_to_object(key, data, content_type);
        }
        if (resp.status == 409 &&
            resp.body.find("PositionNotEqualToLength") != std::string::npos) {
            const std::unordered_map<std::string, std::string>::const_iterator it =
                resp.headers.find("x-oss-next-append-position");
            if (it != resp.headers.end() && !it->second.empty()) {
                position = it->second;
                continue;
            }
        }
        throw StorageError("AliyunOss append_to_object failed (HTTP " + std::to_string(resp.status) + "): " + resp.body);
    }
    throw StorageError("AliyunOss append_to_object: append position kept moving (concurrent appenders)");
}

//------------------------------------------------------------------------------

std::string AliyunOss::get_object(const std::string& key) {
    const std::string fk = full_key(key);
    const std::string date = rfc1123_gmt(std::time(nullptr));

    const std::string string_to_sign =
        "GET\n\n\n" + date + "\n" + canonical_resource(fk);
    const std::string signature = oss_signature(config_.secret_access_key, string_to_sign);

    client::HttpRequest req;
    req.url = object_url(fk);
    req.headers = {
        "Date: " + date,
        "Authorization: OSS " + config_.access_key_id + ":" + signature,
    };
    client::HttpResponse resp = http_.request("GET", req);
    if (resp.status < 200 || resp.status >= 300) {
        throw StorageError("AliyunOss get_object failed (HTTP " + std::to_string(resp.status) + "): " + resp.body);
    }
    return resp.body;
}

//------------------------------------------------------------------------------

std::vector<std::string> AliyunOss::list_objects(const std::string& prefix,
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

    std::vector<std::string> out;
    std::string marker;   // resume point, page 2 onward
    while (out.size() < max_keys) {
        const std::size_t remaining = max_keys - out.size();
        const std::string date = rfc1123_gmt(std::time(nullptr));

        // GetBucket (ListObjects). prefix / marker / max-keys are query
        // parameters, not subresources, so the signed CanonicalizedResource is
        // just the bucket: "/<bucket>/".
        const std::string string_to_sign =
            "GET\n\n\n" + date + "\n/" + config_.bucket + "/";
        const std::string signature = oss_signature(config_.secret_access_key, string_to_sign);

        std::string url = "https://" + host_ + "/?max-keys=" +
            std::to_string(remaining < 1000 ? remaining : 1000);
        if (!fp.empty())     url += "&prefix=" + uri_encode(fp, true);
        if (!marker.empty()) url += "&marker=" + uri_encode(marker, true);

        client::HttpRequest req;
        req.url = url;
        req.headers = {
            "Date: " + date,
            "Authorization: OSS " + config_.access_key_id + ":" + signature,
        };
        client::HttpResponse resp = http_.request("GET", req);
        if (resp.status < 200 || resp.status >= 300) {
            throw StorageError("AliyunOss list_objects failed (HTTP " + std::to_string(resp.status) + "): " + resp.body);
        }

        // <Contents><Key>...</Key>...</Contents> per object; no delimiter is
        // sent, so <Key> only occurs inside <Contents>. The service returns
        // full keys (configured prefix included); strip the prefix so callers
        // get prefix-less keys -- the form every API method returns and accepts
        // (full_key re-applies the prefix internally at access time). The
        // pagination marker stays on the FULL key the service paginates by.
        const std::vector<std::string> keys = xml_tag_values(resp.body, "Key");
        for (std::size_t i = 0; i < keys.size() && out.size() < max_keys; ++i) {
            const std::string& k = keys[i];
            out.push_back((!cfg_prefix.empty() && k.compare(0, cfg_prefix.size(), cfg_prefix) == 0)
                          ? k.substr(cfg_prefix.size()) : k);
        }

        if (xml_tag_value(resp.body, "IsTruncated") != "true") break;
        // NextMarker is only emitted with a delimiter; without one, resume
        // from the last key of this page (the full, service-side key).
        marker = xml_tag_value(resp.body, "NextMarker");
        if (marker.empty() && !keys.empty()) marker = keys.back();
        if (marker.empty()) break;   // defensive: truncated but empty page
    }
    return out;
}

//------------------------------------------------------------------------------

void AliyunOss::delete_object(const std::string& key) {
    const std::string fk = full_key(key);
    const std::string date = rfc1123_gmt(std::time(nullptr));

    const std::string string_to_sign =
        "DELETE\n\n\n" + date + "\n" + canonical_resource(fk);
    const std::string signature = oss_signature(config_.secret_access_key, string_to_sign);

    client::HttpRequest req;
    req.url = object_url(fk);
    req.headers = {
        "Date: " + date,
        "Authorization: OSS " + config_.access_key_id + ":" + signature,
    };
    client::HttpResponse resp = http_.request("DELETE", req);
    // OSS returns 204 for a successful delete and for deleting a missing key.
    if (resp.status != 204 && resp.status != 200 && resp.status != 404) {
        throw StorageError("AliyunOss delete_object failed (HTTP " + std::to_string(resp.status) + "): " + resp.body);
    }
}

//------------------------------------------------------------------------------

std::string AliyunOss::presigned_url(const std::string& key, int expires_seconds) const {
    const std::string fk = full_key(key);
    const std::time_t expires = std::time(nullptr) + expires_seconds;
    const std::string expires_str = std::to_string(static_cast<long long>(expires));

    // For a URL signature the date slot holds the Expires epoch instead.
    const std::string string_to_sign =
        "GET\n\n\n" + expires_str + "\n" + canonical_resource(fk);
    const std::string signature = oss_signature(config_.secret_access_key, string_to_sign);

    return object_url(fk) +
           "?OSSAccessKeyId=" + uri_encode(config_.access_key_id, true) +
           "&Expires=" + expires_str +
           "&Signature=" + uri_encode(signature, true);
}

//------------------------------------------------------------------------------

std::unique_ptr<Storage> OssConfig::open() const {
    return std::unique_ptr<Storage>(new AliyunOss(*this));
}

}}

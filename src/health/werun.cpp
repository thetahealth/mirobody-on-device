#include "health/werun.hpp"

#include "server/auth.hpp"
#include "health/vendor_fhir.hpp"      // vendor_json_to_observations, vendor::DataDomain
#include "fhir/store.hpp"
#include "client/http_client.hpp"
#include "storage/sign.hpp"            // base64_decode
#include "platform/clock.hpp"          // now_unix_ms

#include <openssl/evp.h>
#include <rapidjson/document.h>

#include <cctype>
#include <cstdint>
#include <exception>
#include <string>
#include <vector>

namespace mirobody { namespace health {
namespace {

// Read a string member; "" when absent or not a string.
std::string json_str(const rapidjson::Value& v, const char* key) {
    if (!v.IsObject() || !v.HasMember(key)) return std::string();
    const rapidjson::Value& m = v[key];
    return m.IsString() ? std::string(m.GetString(), m.GetStringLength()) : std::string();
}

// Percent-encode a query-string component (RFC 3986 unreserved pass through).
std::string url_encode(const std::string& s) {
    std::string out;
    out.reserve(s.size() * 3);
    for (unsigned char c : s) {
        if (std::isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') {
            out.push_back(static_cast<char>(c));
        } else {
            static const char* hex = "0123456789ABCDEF";
            out.push_back('%');
            out.push_back(hex[c >> 4]);
            out.push_back(hex[c & 0x0F]);
        }
    }
    return out;
}

// AES-128-CBC decrypt with PKCS7 padding (OpenSSL EVP). `key` and `iv` are the raw
// 16-byte values. Returns "" on any failure (bad padding / wrong key / short input).
std::string aes128cbc_decrypt(const std::string& ct, const std::string& key,
                              const std::string& iv) {
    if (ct.empty() || (ct.size() % 16) != 0) return std::string();
    EVP_CIPHER_CTX* c = EVP_CIPHER_CTX_new();
    if (!c) return std::string();

    std::string out;
    out.resize(ct.size() + 16);
    std::string result;
    int len = 0, total = 0;
    bool ok = false;
    do {
        if (EVP_DecryptInit_ex(c, EVP_aes_128_cbc(), nullptr,
                               reinterpret_cast<const unsigned char*>(key.data()),
                               reinterpret_cast<const unsigned char*>(iv.data())) != 1) break;
        if (EVP_DecryptUpdate(c, reinterpret_cast<unsigned char*>(&out[0]), &len,
                              reinterpret_cast<const unsigned char*>(ct.data()),
                              static_cast<int>(ct.size())) != 1) break;
        total = len;
        if (EVP_DecryptFinal_ex(c, reinterpret_cast<unsigned char*>(&out[0]) + total, &len) != 1) break;
        total += len;
        out.resize(static_cast<std::size_t>(total));
        result.swap(out);
        ok = true;
    } while (false);

    EVP_CIPHER_CTX_free(c);
    return ok ? result : std::string();
}

}  // namespace

//------------------------------------------------------------------------------

WeRunService::WeRunService(server::Router& router, const Config& cfg,
                           database::Database& db, const jwt::Jwt& jwt)
    : cfg_(cfg), db_(db), jwt_(jwt) {
    router.post("/wechat/werun",
                server::require_auth(jwt_, [this](const server::Request& q, server::Response& s){ on_werun(q, s); }));
}

// POST /wechat/werun -- ingest WeChat WeRun step data.
//
// Body:    {"code": "<wx.login code>", "encryptedData": "<b64>", "iv": "<b64>"}
// Success: {"code": 0, "msg": "ok", "data": {"posted": N, "failed": M}}
//
// require_auth guarantees req.user_id > 0 (the mirobody user the steps belong to).
// The WeChat `code` is used only to fetch the session_key that decrypts the blob.
void WeRunService::on_werun(const server::Request& req, server::Response& res) {
    const std::string appid  = cfg_.wechat_appid;
    const std::string secret = cfg_.wechat_secret;
    if (appid.empty() || secret.empty()) {
        res.error(-1, "WeChat Mini Program is not configured (set WECHAT_APPID / WECHAT_SECRET).");
        return;
    }

    rapidjson::Document doc;
    doc.Parse(req.body.c_str(), req.body.size());
    if (doc.HasParseError() || !doc.IsObject()) {
        res.error(-2, "Invalid JSON body.");
        return;
    }
    const std::string code = json_str(doc, "code");
    const std::string enc  = json_str(doc, "encryptedData");
    const std::string iv_b = json_str(doc, "iv");
    if (code.empty() || enc.empty() || iv_b.empty()) {
        res.error(-3, "Missing code / encryptedData / iv.");
        return;
    }

    // Exchange the fresh wx.login() code for the session_key (use-once; not stored).
    const std::string base = cfg_.wechat_api_base.empty() ? std::string("https://api.weixin.qq.com")
                                                          : cfg_.wechat_api_base;
    const std::string url = base + "/sns/jscode2session?appid=" + appid + "&secret=" + secret +
                            "&js_code=" + url_encode(code) + "&grant_type=authorization_code";
    client::HttpResponse r = client::HttpClient().get(url, /*timeout_ms=*/10000);
    if (r.status != 200) {
        res.error(-4, "WeChat code exchange failed (HTTP " + std::to_string(r.status) + ").");
        return;
    }
    rapidjson::Document jr;
    jr.Parse(r.body.c_str(), r.body.size());
    if (jr.HasParseError() || !jr.IsObject()) {
        res.error(-5, "Invalid WeChat response.");
        return;
    }
    if (jr.HasMember("errcode") && jr["errcode"].IsInt() && jr["errcode"].GetInt() != 0) {
        const std::string m = json_str(jr, "errmsg");
        res.error(-6, m.empty() ? std::string("WeChat code exchange error.") : m);
        return;
    }
    const std::string session_key = json_str(jr, "session_key");
    if (session_key.empty()) {
        res.error(-7, "WeChat response has no session_key.");
        return;
    }

    // Decrypt the WeRun blob: AES-128-CBC with base64(session_key) as key + base64(iv).
    const std::string key = storage::base64_decode(session_key);
    const std::string ivb = storage::base64_decode(iv_b);
    const std::string ct  = storage::base64_decode(enc);
    if (key.size() != 16 || ivb.size() != 16) {
        res.error(-8, "Invalid WeChat session key / iv length.");
        return;
    }
    const std::string plain = aes128cbc_decrypt(ct, key, ivb);
    if (plain.empty()) {
        res.error(-9, "WeRun data decryption failed.");
        return;
    }

    rapidjson::Document pd;
    pd.Parse(plain.c_str(), plain.size());
    if (pd.HasParseError() || !pd.IsObject()) {
        res.error(-10, "Invalid WeRun payload.");
        return;
    }
    // Verify the watermark appid matches our Mini Program — WeChat stamps every
    // decrypted payload with the app it was encrypted for, so this rejects a blob
    // captured from a different app.
    rapidjson::Value::ConstMemberIterator wm = pd.FindMember("watermark");
    if (wm != pd.MemberEnd() && wm->value.IsObject()) {
        const std::string wappid = json_str(wm->value, "appid");
        if (!wappid.empty() && wappid != appid) {
            res.error(-11, "WeRun watermark appid mismatch.");
            return;
        }
    }

    // Map the steps to FHIR Observations and persist through the FHIR store.
    const std::string subject = "Patient/" + std::to_string(req.user_id);
    const std::vector<std::string> obs =
        vendor_json_to_observations("werun", vendor::DataDomain::Activity, plain, subject);

    fhir::FhirStore fhir_store(db_);
    const std::int64_t now = platform::now_unix_ms();
    int posted = 0, failed = 0, counter = 0;
    for (std::size_t i = 0; i < obs.size(); ++i) {
        fhir::StoredResource sr;
        sr.type       = "Observation";
        sr.id         = "werun-" + std::to_string(now) + "-" + std::to_string(counter++);
        sr.version_id = 1;
        sr.updated_at = now;
        sr.deleted    = false;
        sr.content    = obs[i];
        try {
            fhir_store.upsert(req.user_id, sr);
            ++posted;
        } catch (const std::exception&) {
            ++failed;
        }
    }

    res.ok("{\"posted\":" + std::to_string(posted) +
           ",\"failed\":" + std::to_string(failed) + "}");
}

}}  // namespace mirobody::health

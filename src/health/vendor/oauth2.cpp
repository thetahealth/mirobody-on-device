#include "health/vendor/oauth2.hpp"

#include "client/http_client.hpp"
#include "storage/sign.hpp"   // base64_encode

#include <rapidjson/document.h>

#include <cctype>
#include <string>

namespace mirobody { namespace vendor {
namespace {

// Percent-encode a form component (RFC 3986 unreserved pass through).
std::string form_encode(const std::string& s) {
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

std::string json_str(const rapidjson::Value& v, const char* key) {
    if (!v.IsObject() || !v.HasMember(key)) return std::string();
    const rapidjson::Value& m = v[key];
    return m.IsString() ? std::string(m.GetString(), m.GetStringLength()) : std::string();
}

}  // namespace

TokenSet oauth2_token_request(const std::string& token_url,
                              const std::vector<std::pair<std::string, std::string> >& params,
                              const std::string& basic_user,
                              const std::string& basic_pass) {
    std::string body;
    for (std::size_t i = 0; i < params.size(); ++i) {
        if (!body.empty()) body.push_back('&');
        body += form_encode(params[i].first) + "=" + form_encode(params[i].second);
    }

    client::HttpRequest rq;
    rq.url          = token_url;
    rq.body         = body;
    rq.content_type = "application/x-www-form-urlencoded";
    rq.headers.push_back("Accept: application/json");
    if (!basic_user.empty()) {
        rq.headers.push_back("Authorization: Basic " +
                             storage::base64_encode(basic_user + ":" + basic_pass));
    }
    rq.request_timeout_ms = 30000;

    client::HttpResponse res = client::HttpClient().post(rq);
    if (res.status < 200 || res.status >= 300) {
        throw VendorError("oauth2 token request failed (HTTP " +
                          std::to_string(res.status) + "): " + res.body.substr(0, 300));
    }

    rapidjson::Document d;
    d.Parse(res.body.c_str(), res.body.size());
    if (d.HasParseError() || !d.IsObject()) {
        throw VendorError("oauth2 token response was not valid JSON");
    }
    TokenSet t;
    t.access_token  = json_str(d, "access_token");
    t.refresh_token = json_str(d, "refresh_token");
    if (d.HasMember("expires_in")) {
        const rapidjson::Value& e = d["expires_in"];
        if (e.IsInt64())      t.expires_in = e.GetInt64();
        else if (e.IsInt())   t.expires_in = e.GetInt();
        else if (e.IsUint())  t.expires_in = e.GetUint();
        else if (e.IsDouble()) t.expires_in = static_cast<std::int64_t>(e.GetDouble());
    }
    if (t.access_token.empty()) {
        throw VendorError("oauth2 token response has no access_token: " + res.body.substr(0, 200));
    }
    return t;
}

}}  // namespace mirobody::vendor

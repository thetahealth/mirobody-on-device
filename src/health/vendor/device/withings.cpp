// Withings (Health Mate) — direct client for a consumer health-device brand,
// normally reachable through an aggregator; this talks to Withings' own API for
// deployments that want it directly. See src/health/README.md.
//
// Withings API (https://developer.withings.com/api-reference/):
//   * base_url defaults to the documented host https://wbsapi.withings.net
//     (override via MIROBODY_VENDOR_WITHINGS_BASE_URL).
//   * auth is an OAuth 2.0 access token (config.api_key) as a Bearer header on
//     fetch(); minted out of band via Withings' authorization-code flow.
//   * authorize_url() builds the standard authorization-code consent URL at
//     account.withings.com/oauth2_user/authorize2 from config.client_id (no
//     network call). revoke() and handle_webhook() stay inherited stubs: Withings
//     documents no app-initiated token-revoke endpoint (only the user can revoke,
//     or Notify's `revoke` action which just drops a webhook subscription), and its
//     Notify webhook carries no inbound signature to verify — the documented
//     pattern is to treat a notification as an untrusted trigger and re-fetch via
//     the Data API.
//   * fetch() POSTs form-encoded requests with an `action` selector, the way the
//     API is shaped. Domains map onto the documented services: body measures
//     (Measure v1 getmeas, unix-second window), heart list (Heart v2, unix-second
//     window), activity and sleep summaries (v2, yyyy-MM-dd window).
// The token already scopes the request to its owner, so `user_id` is accepted for
// interface parity but is not sent.

#include "health/vendor/vendor.hpp"

#include "client/http_client.hpp"

#include <rapidjson/document.h>

#include <cctype>
#include <cstdint>
#include <cstdio>
#include <ctime>
#include <string>
#include <utility>

namespace mirobody { namespace vendor {
namespace {

// OAuth 2.0 authorization endpoint (account host; the Data API lives on the
// wbsapi host that base_url() points at).
const char kAuthorizeUrl[] = "https://account.withings.com/oauth2_user/authorize2";
// Consent scopes for the domains this client brokers (comma-separated per docs).
const char kDefaultScope[] = "user.info,user.metrics,user.activity,user.sleepevents";

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

// ISO-8601 UTC -> Unix epoch seconds; -1 when it doesn't start with a date-time.
// (Measure/Heart take second-resolution windows.)
std::int64_t iso_to_unix_s(const std::string& iso) {
    int y = 0, mo = 0, d = 0, h = 0, mi = 0, s = 0;
    if (std::sscanf(iso.c_str(), "%d-%d-%dT%d:%d:%d", &y, &mo, &d, &h, &mi, &s) < 3) {
        return -1;
    }
    std::tm tm;
    tm.tm_year = y - 1900;
    tm.tm_mon  = mo - 1;
    tm.tm_mday = d;
    tm.tm_hour = h;
    tm.tm_min  = mi;
    tm.tm_sec  = s;
    tm.tm_isdst = 0;
#ifdef _WIN32
    std::time_t t = _mkgmtime(&tm);
#else
    std::time_t t = timegm(&tm);
#endif
    if (t == static_cast<std::time_t>(-1)) return -1;
    return static_cast<std::int64_t>(t);
}

// yyyy-MM-dd prefix, or "" if absent. (Activity/Sleep summaries take day ranges.)
std::string ymd(const std::string& iso) {
    if (iso.size() >= 10 && iso[4] == '-' && iso[7] == '-') return iso.substr(0, 10);
    return std::string();
}

//------------------------------------------------------------------------------

class Withings : public VendorBase {
public:
    explicit Withings(VendorConfig cfg) : VendorBase(make_info(), std::move(cfg)) {}

    std::string fetch(const std::string& /*user_id*/,
                      DataDomain domain,
                      const std::string& start_iso,
                      const std::string& end_iso) override {
        require_token();
        const std::string base = base_url();

        std::string path;
        std::string body;
        switch (domain) {
            case DataDomain::BodyMetrics: {
                const std::int64_t s = iso_to_unix_s(start_iso), e = iso_to_unix_s(end_iso);
                require_window(s, e);
                // meastypes: 1 weight, 9 diastolic BP, 10 systolic BP, 54 SpO2.
                path = "/measure";
                body = "action=getmeas&meastypes=1,9,10,54&category=1"
                       "&startdate=" + std::to_string(s) + "&enddate=" + std::to_string(e);
                break;
            }
            case DataDomain::HeartRate: {
                const std::int64_t s = iso_to_unix_s(start_iso), e = iso_to_unix_s(end_iso);
                require_window(s, e);
                path = "/v2/heart";
                body = "action=list&startdate=" + std::to_string(s) + "&enddate=" + std::to_string(e);
                break;
            }
            case DataDomain::Activity: {
                const std::string s = ymd(start_iso), e = ymd(end_iso);
                require_ymd(s, e);
                path = "/v2/measure";
                body = "action=getactivity&startdateymd=" + s + "&enddateymd=" + e;
                break;
            }
            case DataDomain::Sleep: {
                const std::string s = ymd(start_iso), e = ymd(end_iso);
                require_ymd(s, e);
                path = "/v2/sleep";
                body = "action=getsummary&startdateymd=" + s + "&enddateymd=" + e;
                break;
            }
            default:
                throw VendorError(std::string("withings: unsupported domain '") + to_string(domain) +
                                  "' (Withings brokers: activity, heart_rate, sleep, body_metrics)");
        }

        client::HttpRequest rq;
        rq.url          = base + path;
        rq.body         = std::move(body);
        rq.content_type = "application/x-www-form-urlencoded";
        rq.headers      = { "Authorization: Bearer " + config().api_key };
        rq.request_timeout_ms = 30000;

        client::HttpResponse res = client::HttpClient().post(rq);
        if (res.status < 200 || res.status >= 300) {
            throw VendorError(info_.id + ": fetch failed (HTTP " +
                              std::to_string(res.status) + "): " + res.body.substr(0, 300));
        }
        return res.body;
    }

    // Build the OAuth 2.0 authorization-code consent URL (pure construction; the
    // token is exchanged out of band afterwards). Withings requires redirect_uri
    // and state; user_id/provider are unused. Requires the OAuth client_id. (The
    // authorization code Withings returns is valid for only ~30 seconds.)
    std::string authorize_url(const std::string& redirect_uri,
                              const std::string& state,
                              const std::string& /*user_id*/,
                              const std::string& /*provider*/) override {
        if (config().client_id.empty()) {
            throw VendorError(info_.id + ": authorize_url requires an OAuth client_id — set "
                              "MIROBODY_VENDOR_WITHINGS_CLIENT_ID");
        }
        std::string url = std::string(kAuthorizeUrl) +
                          "?response_type=code&client_id=" + url_encode(config().client_id) +
                          "&scope=" + url_encode(kDefaultScope);
        if (!redirect_uri.empty()) {
            url += "&redirect_uri=" + url_encode(std::string(redirect_uri));
        }
        if (!state.empty()) {
            url += "&state=" + url_encode(std::string(state));
        }
        return url;
    }

    // Withings token exchange is a form POST to {base}/v2/oauth2 with
    // action=requesttoken, wrapped in the standard {status, body:{...}} envelope
    // (not the plain OAuth2 JSON), so it doesn't use the shared helper.
    TokenSet exchange_code(const std::string& code, const std::string& redirect_uri) override {
        return token_action("authorization_code", "code", code, redirect_uri);
    }
    TokenSet refresh(const std::string& refresh_token) override {
        return token_action("refresh_token", "refresh_token", refresh_token, std::string());
    }

private:
    // POST action=requesttoken and unwrap the {status, body:{access_token,...}}
    // envelope. `grant_type` selects exchange vs refresh; (param_name, param_value)
    // carries the code or the refresh token.
    TokenSet token_action(const std::string& grant_type, const char* param_name,
                          const std::string& param_value, const std::string& redirect_uri) {
        if (config().client_id.empty() || config().client_secret.empty()) {
            throw VendorError(info_.id + ": token exchange needs WITHINGS_CLIENT_ID + WITHINGS_CLIENT_SECRET");
        }
        std::string body = "action=requesttoken&grant_type=" + url_encode(grant_type) +
                           "&client_id=" + url_encode(config().client_id) +
                           "&client_secret=" + url_encode(config().client_secret) +
                           "&" + param_name + "=" + url_encode(param_value);
        if (!redirect_uri.empty()) body += "&redirect_uri=" + url_encode(redirect_uri);

        client::HttpRequest rq;
        rq.url          = base_url() + "/v2/oauth2";
        rq.body         = std::move(body);
        rq.content_type = "application/x-www-form-urlencoded";
        rq.headers      = { "Accept: application/json" };
        rq.request_timeout_ms = 30000;

        client::HttpResponse res = client::HttpClient().post(rq);
        if (res.status < 200 || res.status >= 300) {
            throw VendorError(info_.id + ": token request failed (HTTP " +
                              std::to_string(res.status) + "): " + res.body.substr(0, 300));
        }
        rapidjson::Document d;
        d.Parse(res.body.c_str(), res.body.size());
        if (d.HasParseError() || !d.IsObject()) {
            throw VendorError(info_.id + ": token response was not valid JSON");
        }
        // Withings signals errors with a non-zero top-level `status`.
        if (!d.HasMember("status") || !d["status"].IsInt() || d["status"].GetInt() != 0 ||
            !d.HasMember("body") || !d["body"].IsObject()) {
            throw VendorError(info_.id + ": token request error: " + res.body.substr(0, 200));
        }
        const rapidjson::Value& b = d["body"];
        TokenSet t;
        if (b.HasMember("access_token") && b["access_token"].IsString()) {
            t.access_token.assign(b["access_token"].GetString(), b["access_token"].GetStringLength());
        }
        if (b.HasMember("refresh_token") && b["refresh_token"].IsString()) {
            t.refresh_token.assign(b["refresh_token"].GetString(), b["refresh_token"].GetStringLength());
        }
        if (b.HasMember("expires_in")) {
            const rapidjson::Value& e = b["expires_in"];
            if (e.IsInt64())    t.expires_in = e.GetInt64();
            else if (e.IsInt())  t.expires_in = e.GetInt();
            else if (e.IsUint()) t.expires_in = e.GetUint();
        }
        if (t.access_token.empty()) {
            throw VendorError(info_.id + ": token response has no access_token");
        }
        return t;
    }

    static void require_window(std::int64_t s, std::int64_t e) {
        if (s < 0 || e < 0) {
            throw VendorError("withings: this domain needs ISO-8601 start/end bounds "
                              "(unix-second window)");
        }
    }
    static void require_ymd(const std::string& s, const std::string& e) {
        if (s.empty() || e.empty()) {
            throw VendorError("withings: this domain needs ISO-8601 start/end dates (yyyy-MM-dd)");
        }
    }

    std::string base_url() const {
        std::string b = config().base_url;
        if (b.empty()) b = "https://wbsapi.withings.net";
        while (!b.empty() && b.back() == '/') b.pop_back();
        return b;
    }

    void require_token() const {
        if (config().api_key.empty()) {
            throw VendorError(info_.id + ": api_key is required — set "
                              "MIROBODY_VENDOR_WITHINGS_API_KEY to a Withings OAuth 2.0 access token");
        }
    }

    static VendorInfo make_info() {
        VendorInfo i;
        i.id                   = "withings";
        i.display_name         = "Withings Health Mate";
        i.positioning          = "Consumer health-device brand (scales, BP monitors, watches) with an OAuth2 API";
        i.target_customers     = "Apps serving Withings device users";
        i.data_source_coverage = "Withings scales, blood-pressure monitors, sleep mats, watches";
        i.integration_method   = "REST API (OAuth2, form-encoded actions) + notify webhooks";
        i.compliance_summary   = "User-consented OAuth scopes; GDPR (EU)";
        i.differentiator       = "Strong body-measure / blood-pressure / sleep coverage from medical-grade home devices";
        i.docs_url             = "https://developer.withings.com/api-reference/";
        i.region               = Region::EU;
        i.open_source          = false;
        i.compliance           = {"GDPR"};
        i.domains              = {DataDomain::Activity, DataDomain::HeartRate,
                                  DataDomain::Sleep, DataDomain::BodyMetrics};
        i.integrations         = {Integration::Rest, Integration::Webhook};
        return i;
    }
};

}

std::unique_ptr<Vendor> make_withings(const VendorConfig& cfg) {
    return std::unique_ptr<Vendor>(new Withings(cfg));
}

}}

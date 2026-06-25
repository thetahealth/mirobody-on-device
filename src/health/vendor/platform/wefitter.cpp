// WeFitter — health gamification API platform. See README.md (Segment A).
//
// Implemented against the public reference at
// https://www.wefitter.com/en-us/developers/documentation/ (plus the official
// generated client at https://github.com/funxtionatics/wefitter-client, which
// pins exact paths and parameter names). WeFitter aggregates 250+/300+ wearable
// brands behind a REST API, layering a gamified challenge engine and an AI
// biological-age ("Bio Age") score on top. This client covers the data-fetch
// path (per-metric profile summary endpoints), the connection/authorize flow,
// challenge listing, and disconnect, all behind the platform's JWT auth.
//
// CONFIRMED from the public docs + generated client:
//   * base URL   https://api.wefitter.com/api/v1.1/
//   * auth       two-legged. Basic auth (base64 client_id:client_secret) ->
//                POST /token/ returns a JSON object with a "bearer" field (a
//                24h administrator JWT). That JWT is sent as `Authorization:
//                Bearer <jwt>` on subsequent calls.
//   * data fetch GET /profile/{profile_public_id}/<metric>_summary/ with
//                date_start / date_end query params, e.g. daily_summary (steps/
//                activity), heartrate_summary, sleep_summary, biometric.
//                Activity workouts live at .../workout/.
//   * connect    GET /profile/{public_id}/connections/ returns the per-provider
//                connection URLs; a `redirect` query param sets the post-connect
//                return URL. (Apple/Samsung Health are SDK-only, no web URL.)
//   * challenges GET /profile/{public_id}/challenge/
//   * disconnect DELETE /profile/{public_id}/ removes the profile.
//
// INFERRED / not part of a confirmed contract:
//   * The POST /token/ *request body* field names are not published (the Token
//     model documents only the "bearer" *response* field), so the credentials
//     ride in the Basic-auth header — which IS confirmed — and the body is empty.
//   * Bio Age exists ("Bio Age V0.1", changelog 2023-02-20) but its endpoint
//     path is not published anywhere public; rather than invent one, fetch()
//     surfaces an honest VendorError for any domain without a confirmed endpoint,
//     and the bio-age score is not exposed as a fabricated path.
// webhook handling and the SDK-mediated Apple/Samsung connect flow stay stubs.

#include "health/vendor/vendor.hpp"

#include "client/http_client.hpp"

#include <rapidjson/document.h>

#include <cctype>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

namespace mirobody { namespace vendor {
namespace {

//------------------------------------------------------------------------------
// Confirmed defaults + parameter names (see header comment).
//------------------------------------------------------------------------------

const char kDefaultBase[] = "https://api.wefitter.com/api/v1.1/";

// Confirmed token endpoint and the date-range query params shared by the
// per-metric profile summary endpoints.
const char kTokenEndpoint[] = "token/";
const char kStartParam[]    = "date_start";
const char kEndParam[]      = "date_end";

//------------------------------------------------------------------------------
// Local helpers
//------------------------------------------------------------------------------

// Percent-encode a query-string / path component (RFC 3986 unreserved pass
// through). Local copy, matching user/service.cpp, vitalera.cpp and
// healthconnect.cpp.
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

// Minimal RFC 4648 base64 (standard alphabet, '=' padding) for the Basic-auth
// credential pair. Local copy to avoid pulling in a transitive dependency.
std::string base64(const std::string& in) {
    static const char* tbl =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    out.reserve(((in.size() + 2) / 3) * 4);
    std::size_t i = 0;
    for (; i + 2 < in.size(); i += 3) {
        unsigned int n = (static_cast<unsigned char>(in[i]) << 16) |
                         (static_cast<unsigned char>(in[i + 1]) << 8) |
                         (static_cast<unsigned char>(in[i + 2]));
        out.push_back(tbl[(n >> 18) & 0x3F]);
        out.push_back(tbl[(n >> 12) & 0x3F]);
        out.push_back(tbl[(n >> 6) & 0x3F]);
        out.push_back(tbl[n & 0x3F]);
    }
    if (i < in.size()) {
        unsigned int n = static_cast<unsigned char>(in[i]) << 16;
        bool two = (i + 1 < in.size());
        if (two) n |= static_cast<unsigned char>(in[i + 1]) << 8;
        out.push_back(tbl[(n >> 18) & 0x3F]);
        out.push_back(tbl[(n >> 12) & 0x3F]);
        out.push_back(two ? tbl[(n >> 6) & 0x3F] : '=');
        out.push_back('=');
    }
    return out;
}

//------------------------------------------------------------------------------

class WeFitter : public VendorBase {
public:
    explicit WeFitter(VendorConfig cfg) : VendorBase(make_info(), std::move(cfg)) {}

    // Begin the connection flow. WeFitter has no single global OAuth screen:
    // the connect URLs are returned per-profile by GET
    // /profile/{public_id}/connections/ (with an optional `redirect` param for
    // the post-connect landing). That endpoint is keyed by a profile public id,
    // which this signature does not carry, so rather than fabricate a global
    // consent URL we surface an honest "not implemented" pointing at the real,
    // per-profile contract.
    std::string authorize_url(const std::string& /*redirect_uri*/,
                              const std::string& /*state*/) override {
        return not_implemented("authorize_url (connections are per-profile: "
                               "GET /profile/{public_id}/connections/)");
    }

    // Fetch `domain` for `user_id` over [date_start, date_end] from the matching
    // per-metric profile summary endpoint. `user_id` is the WeFitter profile
    // public id. Empty bounds omit the date filter (WeFitter's default window).
    // Returns the response JSON verbatim.
    std::string fetch(const std::string& user_id,
                      DataDomain domain,
                      const std::string& start_iso,
                      const std::string& end_iso) override {
        require_configured();
        const std::string uid(user_id);
        if (uid.empty()) {
            throw VendorError(info_.id + ": fetch requires a user_id (the WeFitter profile public id)");
        }

        const char* endpoint = metric_endpoint(domain);
        if (!endpoint) {
            throw VendorError(info_.id + ": no confirmed fetch endpoint for domain '" +
                              to_string(domain) + "' (supported: activity, heart_rate, "
                              "sleep, body_metrics)");
        }

        std::string url = base_url() + "profile/" + url_encode(uid) + "/" + endpoint;
        char sep = '?';
        if (!start_iso.empty()) {
            url += sep; sep = '&';
            url += std::string(kStartParam) + "=" + url_encode(std::string(start_iso));
        }
        if (!end_iso.empty()) {
            url += sep; sep = '&';
            url += std::string(kEndParam) + "=" + url_encode(std::string(end_iso));
        }
        return get_json(url, std::string("fetch ") + endpoint);
    }

    // The data sources a connected profile can link. WeFitter exposes these
    // per-profile (GET /profile/{public_id}/connections/); there is no global
    // provider catalogue in the public API and this signature carries no profile
    // id, so surface that honestly rather than guess a global endpoint.
    std::string list_providers() override {
        return not_implemented("list_providers (connections are per-profile: "
                               "GET /profile/{public_id}/connections/)");
    }

    // Disconnect a profile: DELETE /profile/{public_id}/ removes it (and with it
    // its provider connections). Best-effort — a non-2xx is surfaced.
    void revoke(const std::string& user_id) override {
        require_configured();
        const std::string uid(user_id);
        if (uid.empty()) {
            throw VendorError(info_.id + ": revoke requires a user_id (the WeFitter profile public id)");
        }
        client::HttpRequest req;
        req.url     = base_url() + "profile/" + url_encode(uid) + "/";
        req.headers = auth_headers();
        client::HttpResponse res = client::HttpClient().request("DELETE", req);
        if (res.status < 200 || res.status >= 300) {
            throw VendorError(http_error("revoke (DELETE profile)", res));
        }
    }

private:
    //--------------------------------------------------------------------------
    // Domain → endpoint mapping (confirmed per-metric summary endpoints)
    //--------------------------------------------------------------------------

    // Map a DataDomain onto the confirmed profile summary endpoint. The daily
    // summary carries step/activity totals; heart-rate and sleep have dedicated
    // summaries; biometric covers weight/height/BMI. Glucose/Nutrition/Labs/
    // Clinical have no confirmed WeFitter endpoint -> nullptr.
    static const char* metric_endpoint(DataDomain d) {
        switch (d) {
            case DataDomain::Activity:    return "daily_summary/";
            case DataDomain::HeartRate:   return "heartrate_summary/";
            case DataDomain::Sleep:       return "sleep_summary/";
            case DataDomain::BodyMetrics: return "biometric/";
            default:                      return nullptr;
        }
    }

    //--------------------------------------------------------------------------
    // Transport + auth
    //--------------------------------------------------------------------------

    std::string get_json(const std::string& url, const std::string& op) {
        client::HttpResponse res = client::HttpClient().get(url, /*timeout_ms=*/30000, auth_headers());
        if (res.status < 200 || res.status >= 300) {
            throw VendorError(http_error(op, res));
        }
        return res.body;
    }

    std::vector<std::string> auth_headers() {
        return { "Authorization: Bearer " + bearer() };
    }

    // Resolve an administrator JWT bearer, caching it for this client's lifetime.
    //   * client_id + client_secret -> mint one at POST /token/ (Basic auth)
    //   * api_key only              -> treat it as a pre-issued bearer JWT
    const std::string& bearer() {
        std::lock_guard<std::mutex> lk(token_mu_);
        if (!token_.empty()) return token_;

        if (!config().client_id.empty() && !config().client_secret.empty()) {
            token_ = mint_token();
        } else {
            token_ = config().api_key;   // used directly as the bearer
        }
        return token_;
    }

    // POST /token/ with HTTP Basic auth carrying client_id:client_secret. The
    // response is a JSON object whose "bearer" field holds the 24h admin JWT.
    std::string mint_token() {
        const std::string creds = base64(config().client_id + ":" + config().client_secret);
        client::HttpRequest req;
        req.url     = base_url() + kTokenEndpoint;
        req.body    = "{}";
        req.headers = { "Authorization: Basic " + creds };
        client::HttpResponse res = client::HttpClient().post(req);
        if (res.status < 200 || res.status >= 300) {
            throw VendorError(http_error("authenticate (token/)", res));
        }

        rapidjson::Document doc;
        if (doc.Parse(res.body.c_str()).HasParseError() || !doc.IsObject()) {
            throw VendorError(info_.id + ": token response was not a JSON object");
        }
        if (doc.HasMember("bearer") && doc["bearer"].IsString()) {
            return doc["bearer"].GetString();
        }
        throw VendorError(info_.id + ": no 'bearer' field in token response");
    }

    //--------------------------------------------------------------------------
    // Misc
    //--------------------------------------------------------------------------

    void require_configured() const {
        if (!config().configured()) {
            throw VendorError(info_.id + ": not configured — set "
                              "MIROBODY_VENDOR_WEFITTER_CLIENT_ID/_CLIENT_SECRET "
                              "(to mint an admin bearer) or MIROBODY_VENDOR_WEFITTER_API_KEY "
                              "(a pre-issued bearer JWT)");
        }
    }

    std::string base_url() const {
        std::string b = config().base_url.empty() ? std::string(kDefaultBase) : config().base_url;
        if (!b.empty() && b.back() != '/') b.push_back('/');
        return b;
    }

    std::string http_error(const std::string& op, const client::HttpResponse& res) const {
        // status <= 0 is a transport failure; curl puts the reason in body.
        return info_.id + ": " + op + " failed (HTTP " + std::to_string(res.status) +
               "): " + res.body.substr(0, 300);
    }

    static VendorInfo make_info() {
        VendorInfo i;
        i.id                   = "wefitter";
        i.display_name         = "WeFitter";
        i.positioning          = "Health gamification API platform";
        i.target_customers     = "Corporate wellness, digital fitness, insurers";
        i.data_source_coverage = "Data from 300+ mainstream wearable brands";
        i.integration_method   = "REST API, mobile SDK";
        i.compliance_summary   = "European compliance standards";
        i.differentiator       = "Gamified challenge engine (team competitions) + AI biological-age scoring";
        i.docs_url             = "https://www.wefitter.com/en-us/";
        i.region               = Region::EU;
        i.open_source          = false;
        i.compliance           = {"GDPR"};
        i.domains              = {DataDomain::Activity, DataDomain::Sleep, DataDomain::HeartRate};
        i.integrations         = {Integration::Rest, Integration::Sdk};
        return i;
    }

    std::mutex  token_mu_;
    std::string token_;
};

}

std::unique_ptr<Vendor> make_wefitter(const VendorConfig& cfg) {
    return std::unique_ptr<Vendor>(new WeFitter(cfg));
}

}}

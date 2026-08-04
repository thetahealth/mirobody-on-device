// Oura (Oura Ring) — direct client for a consumer wearable brand with a free,
// self-serve public cloud API. Like Fitbit/Withings this is a direct integration
// for deployments (or individual developers who own the ring) that want Oura data
// without an aggregator. See src/health/README.md.
//
// Oura API v2 (https://cloud.ouraring.com/v2/docs):
//   * base_url defaults to the documented host https://api.ouraring.com
//     (override via OURA_BASE_URL).
//   * auth is an OAuth 2.0 access token (config.api_key) as a Bearer header on
//     fetch(); the token is exchanged out of band at https://api.ouraring.com/oauth/token.
//     (Personal Access Tokens were deprecated Dec 2025 — OAuth2 only for new apps.)
//   * authorize_url() builds the standard authorization-code consent URL at
//     https://cloud.ouraring.com/oauth/authorize from config.client_id.
//   * fetch() issues the documented v2 usercollection GETs. Daily summaries take a
//     [start_date, end_date] day range (YYYY-MM-DD); the heartrate series takes a
//     [start_datetime, end_datetime] ISO-8601 range instead — handled per domain.
//
// revoke / list_providers / handle_webhook stay inherited stubs: Oura is a single
// brand (no provider catalogue), documents no token-revoke endpoint on the public
// API, and its webhook subscription API is a separate contract not wired here.

#include "health/vendor/vendor.hpp"
#include "health/vendor/oauth2.hpp"

#include "client/http_client.hpp"

#include <cctype>
#include <string>
#include <utility>
#include <vector>

namespace mirobody { namespace vendor {
namespace {

// Authorize lives on cloud.ouraring.com; the API + token live on api.ouraring.com.
const char kAuthorizeUrl[] = "https://cloud.ouraring.com/oauth/authorize";
// Consent scopes matching the domains this client brokers (space-separated).
const char kDefaultScope[] = "daily heartrate personal";

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

// yyyy-MM-dd prefix of an ISO-8601 timestamp, or "" if it does not start with a date.
std::string ymd(const std::string& iso) {
    if (iso.size() >= 10 &&
        std::isdigit(static_cast<unsigned char>(iso[0])) && iso[4] == '-' && iso[7] == '-') {
        return iso.substr(0, 10);
    }
    return std::string();
}

//------------------------------------------------------------------------------

class Oura : public VendorBase {
public:
    explicit Oura(VendorConfig cfg) : VendorBase(make_info(), std::move(cfg)) {}

    // Fetch `domain` over [start_iso, end_iso] as the Oura v2 JSON. The token
    // scopes the request to its owner, so `user_id` is unused (accepted for parity).
    std::string fetch(const std::string& /*user_id*/,
                      DataDomain domain,
                      const std::string& start_iso,
                      const std::string& end_iso) override {
        require_token();
        const std::string url = base_url() + endpoint(domain, start_iso, end_iso);
        const std::vector<std::string> headers = {
            "Authorization: Bearer " + config().api_key,
            "Accept: application/json",
        };
        client::HttpResponse res = client::HttpClient().get(url, /*timeout_ms=*/30000, headers);
        if (res.status < 200 || res.status >= 300) {
            throw VendorError(info_.id + ": fetch failed (HTTP " +
                              std::to_string(res.status) + "): " + res.body.substr(0, 300));
        }
        return res.body;
    }

    // Build the OAuth 2.0 authorization-code consent URL (pure construction; the
    // token is exchanged out of band). Requires the OAuth client_id.
    std::string authorize_url(const std::string& redirect_uri,
                              const std::string& state,
                              const std::string& /*user_id*/,
                              const std::string& /*provider*/) override {
        if (config().client_id.empty()) {
            throw VendorError(info_.id + ": authorize_url requires an OAuth client_id — set "
                              "OURA_CLIENT_ID (config or env)");
        }
        std::string url = std::string(kAuthorizeUrl) +
                          "?response_type=code&client_id=" + url_encode(config().client_id) +
                          "&scope=" + url_encode(kDefaultScope);
        if (!redirect_uri.empty()) url += "&redirect_uri=" + url_encode(redirect_uri);
        if (!state.empty())        url += "&state=" + url_encode(state);
        return url;
    }

    // OAuth2 token endpoint at {base}/oauth/token; client credentials in the body.
    TokenSet exchange_code(const std::string& code, const std::string& redirect_uri) override {
        require_client();
        return oauth2_token_request(base_url() + "/oauth/token", {
            {"grant_type", "authorization_code"},
            {"code", code},
            {"redirect_uri", redirect_uri},
            {"client_id", config().client_id},
            {"client_secret", config().client_secret},
        });
    }
    TokenSet refresh(const std::string& refresh_token) override {
        require_client();
        return oauth2_token_request(base_url() + "/oauth/token", {
            {"grant_type", "refresh_token"},
            {"refresh_token", refresh_token},
            {"client_id", config().client_id},
            {"client_secret", config().client_secret},
        });
    }

private:
    void require_client() const {
        if (config().client_id.empty() || config().client_secret.empty()) {
            throw VendorError(info_.id + ": token exchange needs OURA_CLIENT_ID + OURA_CLIENT_SECRET");
        }
    }

    // v2 usercollection paths. Daily summaries use start_date/end_date (YYYY-MM-DD);
    // the heartrate series uses start_datetime/end_datetime (ISO-8601). Throws for
    // domains Oura does not broker.
    static std::string endpoint(DataDomain d, const std::string& start_iso,
                                const std::string& end_iso) {
        switch (d) {
            case DataDomain::Activity: {
                const std::string s = require_date(start_iso), e = require_date(end_iso);
                return "/v2/usercollection/daily_activity?start_date=" + s + "&end_date=" + e;
            }
            case DataDomain::Sleep: {
                const std::string s = require_date(start_iso), e = require_date(end_iso);
                return "/v2/usercollection/daily_sleep?start_date=" + s + "&end_date=" + e;
            }
            case DataDomain::HeartRate: {
                if (start_iso.empty() || end_iso.empty()) {
                    throw VendorError("oura: heartrate fetch requires ISO-8601 start/end datetimes");
                }
                return "/v2/usercollection/heartrate?start_datetime=" + url_encode(start_iso) +
                       "&end_datetime=" + url_encode(end_iso);
            }
            default:
                throw VendorError(std::string("oura: unsupported domain '") + to_string(d) +
                                  "' (Oura brokers: activity, sleep, heart_rate)");
        }
    }

    static std::string require_date(const std::string& iso) {
        const std::string d = ymd(iso);
        if (d.empty()) {
            throw VendorError("oura: fetch requires ISO-8601 start/end dates (YYYY-MM-DD day range)");
        }
        return d;
    }

    std::string base_url() const {
        std::string b = config().base_url;
        if (b.empty()) b = "https://api.ouraring.com";
        while (!b.empty() && b.back() == '/') b.pop_back();
        return b;
    }

    void require_token() const {
        if (config().api_key.empty()) {
            throw VendorError(info_.id + ": api_key is required — set "
                              "OURA_API_KEY (config or env) to an Oura OAuth 2.0 access token");
        }
    }

    static VendorInfo make_info() {
        VendorInfo i;
        i.id                   = "oura";
        i.display_name         = "Oura";
        i.positioning          = "Smart-ring wearable brand with a free, self-serve OAuth2 cloud API";
        i.target_customers     = "Apps (and individual developers) serving Oura Ring users";
        i.data_source_coverage = "Oura Ring: sleep, readiness, activity, heart rate / HRV";
        i.integration_method   = "REST v2 Web API (OAuth2)";
        i.compliance_summary   = "User-consented OAuth scopes";
        i.differentiator       = "Free self-serve API (<=10 users before approval), developer-friendly";
        i.docs_url             = "https://cloud.ouraring.com/v2/docs";
        i.region               = Region::Global;
        i.open_source          = false;
        i.domains              = {DataDomain::Sleep, DataDomain::Activity, DataDomain::HeartRate};
        i.integrations         = {Integration::Rest};
        return i;
    }
};

}

std::unique_ptr<Vendor> make_oura(const VendorConfig& cfg) {
    return std::unique_ptr<Vendor>(new Oura(cfg));
}

}}

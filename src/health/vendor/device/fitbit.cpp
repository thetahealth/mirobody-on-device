// Fitbit (Google) — direct client for a consumer wearable brand. Brands like
// this are normally reached through an aggregator (Terra/Validic/…); this is a
// direct integration for deployments that want to talk to Fitbit's public Web
// API without one. See src/health/README.md.
//
// Fitbit Web API (https://dev.fitbit.com/build/reference/web-api/):
//   * base_url defaults to the documented host https://api.fitbit.com
//     (override via MIROBODY_VENDOR_FITBIT_BASE_URL).
//   * auth is an OAuth 2.0 access token (config.api_key) as a Bearer header;
//     the token is minted out of band via Fitbit's authorization-code + PKCE
//     flow, so authorize_url / revoke stay stubs here.
//   * fetch() issues the documented per-domain time-series GETs over a
//     [startDate, endDate] day range (yyyy-MM-dd). The user path segment is the
//     Fitbit user id, or "-" (the API's alias for the token's own user).

#include "health/vendor/vendor.hpp"

#include "client/http_client.hpp"

#include <cctype>
#include <string>
#include <utility>

namespace mirobody { namespace vendor {
namespace {

// yyyy-MM-dd prefix of an ISO-8601 timestamp, or "" if it does not start with a
// date. Fitbit time-series endpoints take whole days, not instants.
std::string ymd(const std::string& iso) {
    if (iso.size() >= 10 &&
        std::isdigit(static_cast<unsigned char>(iso[0])) && iso[4] == '-' && iso[7] == '-') {
        return iso.substr(0, 10);
    }
    return std::string();
}

//------------------------------------------------------------------------------

class Fitbit : public VendorBase {
public:
    explicit Fitbit(VendorConfig cfg) : VendorBase(make_info(), std::move(cfg)) {}

    // Fetch `domain` for `user_id` over [start_iso, end_iso] as the Fitbit JSON.
    // `user_id` is the Fitbit user id; empty falls back to "-" (the token owner).
    std::string fetch(const std::string& user_id,
                      DataDomain domain,
                      const std::string& start_iso,
                      const std::string& end_iso) override {
        require_token();
        const std::string base = base_url();
        const std::string u = user_id.empty() ? std::string("-") : user_id;

        const std::string s = ymd(start_iso);
        const std::string e = ymd(end_iso);
        if (s.empty() || e.empty()) {
            throw VendorError(info_.id + ": fetch requires ISO-8601 start/end dates (yyyy-MM-dd day range)");
        }

        const std::string url = base + endpoint(domain, u, s, e);
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

private:
    // Documented Web API time-series paths. Sleep is served by API v1.2; the rest
    // by v1. Throws for domains Fitbit does not broker.
    static std::string endpoint(DataDomain d, const std::string& u,
                                const std::string& s, const std::string& e) {
        switch (d) {
            case DataDomain::Activity:
                return "/1/user/" + u + "/activities/steps/date/" + s + "/" + e + ".json";
            case DataDomain::HeartRate:
                return "/1/user/" + u + "/activities/heart/date/" + s + "/" + e + ".json";
            case DataDomain::Sleep:
                return "/1.2/user/" + u + "/sleep/date/" + s + "/" + e + ".json";
            case DataDomain::BodyMetrics:
                return "/1/user/" + u + "/body/log/weight/date/" + s + "/" + e + ".json";
            default:
                throw VendorError(std::string("fitbit: unsupported domain '") + to_string(d) +
                                  "' (Fitbit brokers: activity, heart_rate, sleep, body_metrics)");
        }
    }

    std::string base_url() const {
        std::string b = config().base_url;
        if (b.empty()) b = "https://api.fitbit.com";
        while (!b.empty() && b.back() == '/') b.pop_back();
        return b;
    }

    void require_token() const {
        if (config().api_key.empty()) {
            throw VendorError(info_.id + ": api_key is required — set "
                              "MIROBODY_VENDOR_FITBIT_API_KEY to a Fitbit OAuth 2.0 access token");
        }
    }

    static VendorInfo make_info() {
        VendorInfo i;
        i.id                   = "fitbit";
        i.display_name         = "Fitbit";
        i.positioning          = "Consumer wearable brand (Google) with a public OAuth2 Web API";
        i.target_customers     = "Apps serving Fitbit tracker / smartwatch users";
        i.data_source_coverage = "Fitbit trackers and smartwatches";
        i.integration_method   = "REST Web API (OAuth2) + subscription webhooks";
        i.compliance_summary   = "User-consented OAuth scopes";
        i.differentiator       = "Mature, public, well-documented consumer wearable API";
        i.docs_url             = "https://dev.fitbit.com/build/reference/web-api/";
        i.region               = Region::Global;
        i.open_source          = false;
        i.domains              = {DataDomain::Activity, DataDomain::HeartRate,
                                  DataDomain::Sleep, DataDomain::BodyMetrics};
        i.integrations         = {Integration::Rest, Integration::Webhook};
        return i;
    }
};

}

std::unique_ptr<Vendor> make_fitbit(const VendorConfig& cfg) {
    return std::unique_ptr<Vendor>(new Fitbit(cfg));
}

}}

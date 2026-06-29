// Open Wearables — open-source self-hosted wearable API platform. See README.md
// (Strategic Option A).
//
// Open Wearables (by The Momentum, https://github.com/the-momentum/open-wearables)
// is an MIT-licensed, self-hosted FastAPI service that unifies wearable data
// (Apple Health, Samsung/Google Health Connect, Garmin, Polar, Oura, Whoop,
// Fitbit, …) behind one normalized REST API. Because every deployment serves a
// single organization on the operator's own infrastructure, there is NO public
// host: base_url is REQUIRED and points at the operator's server (e.g.
// http://localhost:8000) — we refuse rather than guess one.
//
// CONFIRMED from the public docs (https://openwearables.io/docs, API Reference):
//   * base path           /api/v1 under the deployer's host
//   * auth                a custom API-key header, NOT a bearer token:
//                             X-Open-Wearables-API-Key: <key>
//                         (the docs explicitly say "Do not use Bearer token
//                          format … a custom header, not the standard
//                          Authorization header"). We send config.api_key here.
//   * per-user summaries  GET /api/v1/users/{user_id}/summaries/{activity|sleep|body|recovery}
//   * timeseries          GET /api/v1/users/{user_id}/timeseries (granular series)
//   * date range params   start_date / end_date, ISO-8601 accepted
//   * providers list      GET /api/v1/oauth/providers
//
// INFERRED — the per-endpoint field reference (the Swagger UI / openapi.json
// served by a running deployment) is not statically published, so the timeseries
// metric-selector parameter name and its heart-rate value are best-effort and
// centralized in the constants below for easy reconciliation against a live
// /docs. The summary paths, the date params, and the providers path are
// confirmed; only the timeseries metric selector is a guess.
//
// Mapped operations: fetch() (Activity→activity summary, Sleep→sleep summary,
// HeartRate→timeseries), list_providers() (GET oauth/providers — a global
// catalogue, so the user_id arg is ignored), and authorize_url(), which builds the
// per-provider consent URL GET /api/v1/oauth/{provider}/authorize?user_id=… now
// that the interface carries provider + user_id (the endpoint takes neither
// redirect_uri nor state, so those are ignored). revoke() / handle_webhook() stay
// inherited stubs: there is no documented disconnect endpoint, and the webhook
// signature scheme is not published.

#include "health/vendor/vendor.hpp"

#include "client/http_client.hpp"

#include <cctype>
#include <string>
#include <utility>
#include <vector>

namespace mirobody { namespace vendor {
namespace {

//------------------------------------------------------------------------------
// Confirmed paths + inferred names (see header comment).
//------------------------------------------------------------------------------

const char kApiBase[] = "api/v1/";

// Confirmed: custom API-key header (not Authorization/Bearer).
const char kApiKeyHeader[] = "X-Open-Wearables-API-Key: ";

// Confirmed: date-range query parameters (ISO-8601 accepted).
const char kStartParam[] = "start_date";
const char kEndParam[]   = "end_date";

// Inferred: the timeseries metric-selector query param and its heart-rate value.
// The summaries below are confirmed paths; only this selector is unverified
// against a live /docs (openapi.json).
const char kTimeseriesMetricParam[] = "metric";
const char kHeartRateMetric[]       = "heart_rate";

//------------------------------------------------------------------------------
// Local helpers
//------------------------------------------------------------------------------

// Percent-encode a query-string component (RFC 3986 unreserved pass through).
// Local copy, matching user/service.cpp, vitalera.cpp and healthconnect.cpp.
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

//------------------------------------------------------------------------------

class OpenWearables : public VendorBase {
public:
    explicit OpenWearables(VendorConfig cfg) : VendorBase(make_info(), std::move(cfg)) {}

    // Begin consent for one provider. Open Wearables' authorize endpoint IS the URL
    // the user visits to start OAuth — GET /api/v1/oauth/{provider}/authorize
    // ?user_id=<id> — so we build and return it directly (no API call). `provider`
    // (the cloud provider slug) and `user_id` are both required; the endpoint takes
    // neither redirect_uri nor state, so those are ignored.
    std::string authorize_url(const std::string& /*redirect_uri*/,
                              const std::string& /*state*/,
                              const std::string& user_id,
                              const std::string& provider) override {
        const std::string base = require_base_url();
        const std::string uid(user_id);
        if (uid.empty()) {
            throw VendorError(info_.id + ": authorize_url requires a user_id (the Open Wearables user id)");
        }
        if (provider.empty()) {
            throw VendorError(info_.id + ": authorize_url requires a provider (the cloud provider "
                              "slug; the authorize endpoint is per provider)");
        }
        return base + "oauth/" + url_encode(provider) + "/authorize?user_id=" + url_encode(uid);
    }

    // The cloud providers (device brands) a user can connect, as the platform's
    // JSON. Confirmed path: GET /api/v1/oauth/providers (global catalogue, so the
    // user_id argument is ignored).
    std::string list_providers(const std::string& /*user_id*/) override {
        const std::string base = require_base_url();
        require_api_key();
        return get_json(base + "oauth/providers", "list_providers (oauth/providers)");
    }

    // Fetch `domain` for `user_id` over [start_iso, end_iso], as the platform's
    // normalized JSON. Activity/Sleep hit the confirmed per-user summary
    // endpoints; HeartRate hits the timeseries endpoint (metric selector
    // inferred). Empty bounds => the platform's default window (params omitted).
    std::string fetch(const std::string& user_id,
                      DataDomain domain,
                      const std::string& start_iso,
                      const std::string& end_iso) override {
        const std::string base = require_base_url();
        require_api_key();

        const std::string uid(user_id);
        if (uid.empty()) {
            throw VendorError(info_.id + ": fetch requires a user_id (the Open Wearables user id)");
        }

        const std::string user_path = base + "users/" + url_encode(uid) + "/";

        std::string url;
        std::string op;
        if (domain == DataDomain::HeartRate) {
            // Confirmed path; metric selector inferred (see constants block).
            url = user_path + "timeseries?" + kTimeseriesMetricParam + "=" + kHeartRateMetric;
            op  = "fetch timeseries";
        } else if (const char* summary = summary_path(domain)) {
            url = user_path + "summaries/" + summary;
            op  = std::string("fetch summaries/") + summary;
        } else {
            throw VendorError(std::string(info_.id) + ": unsupported domain '" +
                              to_string(domain) +
                              "' (configured domains: activity, sleep, heart_rate)");
        }

        url += (url.find('?') == std::string::npos) ? '?' : '&';
        // Drop the trailing separator if no range params follow.
        bool has_param = false;
        if (!start_iso.empty()) {
            url += std::string(kStartParam) + "=" + url_encode(std::string(start_iso));
            has_param = true;
        }
        if (!end_iso.empty()) {
            if (has_param) url += "&";
            url += std::string(kEndParam) + "=" + url_encode(std::string(end_iso));
            has_param = true;
        }
        if (!has_param && (url.back() == '?' || url.back() == '&')) {
            url.pop_back();  // nothing was appended after the separator
        }

        return get_json(url, op);
    }

private:
    // Map a DataDomain onto its confirmed per-user summary path segment. Returns
    // nullptr for domains with no summary endpoint (HeartRate is handled via
    // timeseries before this is consulted).
    static const char* summary_path(DataDomain d) {
        switch (d) {
            case DataDomain::Activity:    return "activity";
            case DataDomain::Sleep:       return "sleep";
            case DataDomain::BodyMetrics: return "body";
            default:                      return nullptr;
        }
    }

    //--------------------------------------------------------------------------
    // Transport + auth
    //--------------------------------------------------------------------------

    std::string get_json(const std::string& url, const std::string& op) {
        const std::vector<std::string> headers = {
            std::string(kApiKeyHeader) + config().api_key,
            "Accept: application/json",
        };
        client::HttpResponse res = client::HttpClient().get(url, /*timeout_ms=*/30000, headers);
        if (res.status < 200 || res.status >= 300) {
            throw VendorError(info_.id + ": " + op + " failed (HTTP " +
                              std::to_string(res.status) + "): " + res.body.substr(0, 300));
        }
        return res.body;
    }

    // The deployer's Open Wearables host, normalized to a trailing slash + the
    // confirmed /api/v1/ base path. Required: it is self-hosted, so there is no
    // public host to default to.
    std::string require_base_url() const {
        std::string b = config().base_url;
        if (b.empty()) {
            throw VendorError(info_.id + ": base_url is required — set "
                              "MIROBODY_VENDOR_OPEN_WEARABLES_BASE_URL to your self-hosted "
                              "Open Wearables host (e.g. http://localhost:8000); it is "
                              "self-hosted, so there is no public host to default to");
        }
        if (b.back() != '/') b.push_back('/');
        return b + kApiBase;
    }

    void require_api_key() const {
        if (config().api_key.empty()) {
            throw VendorError(info_.id + ": api_key is required — set "
                              "MIROBODY_VENDOR_OPEN_WEARABLES_API_KEY to an API key created "
                              "in your deployment's developer portal (sent as the "
                              "X-Open-Wearables-API-Key header, not a bearer token)");
        }
    }

    static VendorInfo make_info() {
        VendorInfo i;
        i.id                   = "open_wearables";
        i.display_name         = "Open Wearables";
        i.positioning          = "Open-source self-hosted wearable API platform";
        i.target_customers     = "Startups, growth-stage companies, indie developers";
        i.data_source_coverage = "Apple Health, Samsung, Garmin, Polar, and more";
        i.integration_method   = "Flutter/React Native SDK, AI-ready endpoints";
        i.compliance_summary   = "Self-hosted control (compliance depends on deployment environment)";
        i.differentiator       = "Open-source and free, no SaaS seat fees, built-in AI interface (can connect to Claude)";
        i.docs_url             = "https://www.themomentum.ai";
        i.region               = Region::SelfHosted;
        i.open_source          = true;
        i.domains              = {DataDomain::Activity, DataDomain::Sleep, DataDomain::HeartRate};
        i.integrations         = {Integration::Sdk, Integration::Rest};
        return i;
    }
};

}

std::unique_ptr<Vendor> make_open_wearables(const VendorConfig& cfg) {
    return std::unique_ptr<Vendor>(new OpenWearables(cfg));
}

}}

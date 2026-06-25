// Huawei Health Kit — the one device-native health platform with a server-side
// cloud REST API (Apple HealthKit, Google Health Connect, and Xiaomi/Mi Fitness
// are on-device only; their data reaches mirobody by being read on the phone and
// POSTed to the FHIR endpoint, not fetched here — see src/health/README.md).
//
// Huawei exposes two surfaces: an on-device Health Kit SDK (Android/HarmonyOS)
// and a Health Kit *Cloud* REST API a server can call with a user's OAuth token.
// This client implements the cloud REST read path:
//   * base_url defaults to the documented host https://health-api.cloud.huawei.com
//     (override via MIROBODY_VENDOR_HUAWEI_BASE_URL for a regional endpoint).
//   * auth is a Huawei Account Kit OAuth 2.0 access token (config.api_key) carrying
//     Health Kit scopes; token acquisition / refresh runs out of band against
//     Huawei's authorization server (oauth-login.cloud.huawei.com), which is why
//     authorize_url / revoke stay stubs rather than half-built flows.
//   * fetch() issues the documented `sampleSet:polymerize` query, which aggregates
//     a data type's sample points over [startTime, endTime] (epoch ms) into daily
//     buckets. DataDomains map onto Huawei's `com.huawei.*` atomic data types.
// The consent OAuth flow and the subscription/webhook push are not implemented:
// those contracts need the deployer's client_id + registered scopes, so they
// throw "not implemented" rather than fabricate a flow. See the Health Kit REST
// API reference at https://developer.huawei.com/consumer/en/hms/huawei-healthkit/.

#include "health/vendor/vendor.hpp"

#include "client/http_client.hpp"

#include <cstdint>
#include <cstdio>
#include <ctime>
#include <string>
#include <utility>

namespace mirobody { namespace vendor {
namespace {

//------------------------------------------------------------------------------
// Local helpers
//------------------------------------------------------------------------------

// Convert an ISO-8601 UTC timestamp ("YYYY-MM-DDTHH:MM:SS", with any fractional
// seconds and 'Z'/offset suffix ignored and treated as UTC) to Unix epoch
// milliseconds. Returns -1 when the string does not begin with a parseable
// date-time. Huawei's polymerize body wants epoch-ms bounds, unlike the FHIR
// clients that pass ISO straight through.
std::int64_t iso_to_unix_ms(const std::string& iso) {
    int y = 0, mo = 0, d = 0, h = 0, mi = 0, s = 0;
    // Date is required; time defaults to 00:00:00 when absent.
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
    return static_cast<std::int64_t>(t) * 1000;
}

//------------------------------------------------------------------------------

class Huawei : public VendorBase {
public:
    explicit Huawei(VendorConfig cfg) : VendorBase(make_info(), std::move(cfg)) {}

    // Fetch `domain` for `user_id` over [start_iso, end_iso] as the Huawei Health
    // Kit polymerize JSON. The OAuth token already scopes the request to its
    // owner, so `user_id` is not part of the cloud query (data belongs to the
    // token holder); it is accepted for interface parity and must be non-empty.
    std::string fetch(const std::string& user_id,
                      DataDomain domain,
                      const std::string& start_iso,
                      const std::string& end_iso) override {
        const std::string base = base_url();
        require_token();

        if (user_id.empty()) {
            throw VendorError(info_.id + ": fetch requires a user_id (the linked Huawei account)");
        }
        const char* data_type = huawei_data_type(domain);

        const std::int64_t start_ms = iso_to_unix_ms(start_iso);
        const std::int64_t end_ms   = iso_to_unix_ms(end_iso);
        if (start_ms < 0 || end_ms < 0) {
            throw VendorError(info_.id + ": fetch requires ISO-8601 start/end bounds "
                              "(polymerize needs an explicit [startTime, endTime] window)");
        }

        // sampleSet:polymerize — aggregate the data type's sample points over the
        // window into daily (86,400,000 ms) buckets. Values are controlled (a
        // fixed data-type string and two integers), so the body is concatenated.
        std::string body = std::string("{")
            + "\"polymerizeWith\":[{\"dataTypeName\":\"" + data_type + "\"}],"
            + "\"startTime\":" + std::to_string(start_ms) + ","
            + "\"endTime\":"   + std::to_string(end_ms)   + ","
            + "\"groupByTime\":{\"duration\":86400000}"
            + "}";

        client::HttpRequest rq;
        rq.url          = base + "healthkit/v1/sampleSet:polymerize";
        rq.body         = std::move(body);
        rq.content_type = "application/json";
        rq.headers      = { "Authorization: Bearer " + config().api_key };
        rq.request_timeout_ms = 30000;

        client::HttpResponse res = client::HttpClient().post(rq);
        if (res.status < 200 || res.status >= 300) {
            throw VendorError(info_.id + ": polymerize failed (HTTP " +
                              std::to_string(res.status) + "): " + res.body.substr(0, 300));
        }
        return res.body;
    }

private:
    // Map a DataDomain onto a Huawei Health Kit atomic data type name
    // (developer.huawei.com Health Kit REST API > Data Types; naming follows
    // com.huawei.<continuous|instantaneous>.<metric>). Throws for domains Huawei
    // Health does not broker (Nutrition / Labs / Clinical).
    static const char* huawei_data_type(DataDomain d) {
        switch (d) {
            case DataDomain::Activity:    return "com.huawei.continuous.step.count";
            case DataDomain::HeartRate:   return "com.huawei.continuous.heart_rate";
            case DataDomain::Sleep:       return "com.huawei.continuous.sleep";
            case DataDomain::Glucose:     return "com.huawei.instantaneous.blood_glucose";
            case DataDomain::BodyMetrics: return "com.huawei.instantaneous.body.weight";
            default:
                throw VendorError(std::string("huawei: unsupported domain '") + to_string(d) +
                                  "' (Huawei Health brokers: activity, heart_rate, sleep, "
                                  "glucose, body_metrics)");
        }
    }

    // The Health Kit cloud host, normalized to a trailing slash. Defaults to the
    // documented global host; override for a regional endpoint.
    std::string base_url() const {
        std::string b = config().base_url;
        if (b.empty()) b = "https://health-api.cloud.huawei.com";
        if (b.back() != '/') b.push_back('/');
        return b;
    }

    void require_token() const {
        if (config().api_key.empty()) {
            throw VendorError(info_.id + ": api_key is required — set "
                              "MIROBODY_VENDOR_HUAWEI_API_KEY to a Huawei Account Kit OAuth 2.0 "
                              "access token carrying Health Kit scopes (minted out of band)");
        }
    }

    static VendorInfo make_info() {
        VendorInfo i;
        i.id                   = "huawei";
        i.display_name         = "Huawei Health Kit";
        i.positioning          = "Device-native wearable platform with a server-side cloud REST API";
        i.target_customers     = "Apps serving Huawei / Honor device users";
        i.data_source_coverage = "Huawei Health app: Huawei/Honor wearables and phone sensors";
        i.integration_method   = "Health Kit Cloud REST API (OAuth 2.0 via Huawei Account Kit) + on-device Health Kit SDK";
        i.compliance_summary   = "User-consented OAuth scopes; GDPR (EU) / PIPL (CN)";
        i.differentiator       = "The one major device-native platform offering a server-to-server cloud REST API (Apple/Xiaomi are on-device only)";
        i.docs_url             = "https://developer.huawei.com/consumer/en/hms/huawei-healthkit/";
        i.region               = Region::Global;
        i.open_source          = false;
        i.compliance           = {"GDPR"};
        i.domains              = {DataDomain::Activity, DataDomain::HeartRate, DataDomain::Sleep,
                                  DataDomain::Glucose, DataDomain::BodyMetrics};
        i.integrations         = {Integration::Rest, Integration::Sdk};
        return i;
    }
};

}

std::unique_ptr<Vendor> make_huawei(const VendorConfig& cfg) {
    return std::unique_ptr<Vendor>(new Huawei(cfg));
}

}}

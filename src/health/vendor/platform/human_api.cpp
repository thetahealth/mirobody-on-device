// Human API — consumer-controlled health data aggregator. See README.md
// (Segment C).
//
// Implemented against the public reference at https://reference.humanapi.co — a
// consumer-mediated aggregator that brokers wearable/"wellness" data plus
// clinical EHR ("Medical") records from a large share of U.S. hospitals, behind
// a unified RESTful Data API and the Human Connect authorization widget.
// (Human API was acquired by LexisNexis; the v2.x developer portal layers an
// order-management "Health Intelligence Platform" on top of the classic Data
// API, but the v1 Data API documented below is the consumer-data path this
// client targets.)
//
// CONFIRMED from the public docs:
//   * Data API base       https://api.humanapi.co/v1/human
//   * auth                user access token as "Authorization: Bearer <token>";
//                         the token is config.api_key here. It is minted out of
//                         band by exchanging a Human Connect sessionTokenObject
//                         (+ client_secret) at POST https://user.humanapi.co/
//                         v1/connect/tokens, which returns {humanId, accessToken,
//                         publicToken}. That exchange needs the browser-side
//                         widget's session object, so it is not reproduced here.
//   * wellness endpoints  GET /activities, /sleeps, /heart_rate, /blood_glucose,
//                         /weight, /bmi, /blood_pressure under the base above
//   * time filtering      since / until (ISO-8601), plus updated_since for
//                         incremental sync (Patterns & Conventions reference)
//   * Human Connect       a CLIENT-SIDE JavaScript popup (connect.humanapi.co/
//                         connect.js, HumanConnect.open({clientId, clientUserId,
//                         publicToken})) — there is NO server-issued hosted
//                         redirect/authorize URL, so authorize_url() cannot honor
//                         the redirect_uri/state contract and stays a stub.
//
// INFERRED — the field-level Medical (Clinical) API reference is gated behind the
// developer portal post-acquisition, so the clinical resource path below is a
// best-effort placeholder centralized in the constants block; the wellness paths
// and the Bearer/since/until conventions above are documented, not guessed.
//
// STUBS (honest "not implemented", like healthconnect.cpp): authorize_url (no
// server-side redirect contract — widget is client-side JS), list_providers,
// handle_webhook, and revoke — none have a publicly documented contract for the
// v1 Data API, and fabricating one would be worse than reporting "not implemented".

#include "health/vendor/vendor.hpp"

#include "client/http_client.hpp"

#include <cctype>
#include <string>
#include <utility>
#include <vector>

namespace mirobody { namespace vendor {
namespace {

//------------------------------------------------------------------------------
// Confirmed defaults + one inferred path (see header comment).
//------------------------------------------------------------------------------

// Confirmed public Data API host + version + segment.
const char kDefaultBase[] = "https://api.humanapi.co/v1/human/";

// Confirmed wellness resource paths (relative to kDefaultBase).
const char kPathActivities[]    = "activities";
const char kPathSleeps[]        = "sleeps";
const char kPathHeartRate[]     = "heart_rate";
const char kPathBloodGlucose[]  = "blood_glucose";
const char kPathBloodPressure[] = "blood_pressure";

// Confirmed time-filter params (ISO-8601 values); updated_since drives the
// incremental-sync pattern, since/until bound an explicit window.
const char kSinceParam[]   = "since";
const char kUntilParam[]   = "until";

// Inferred: the Medical (Clinical) API resource path. The wellness paths above
// are documented; the field-level clinical reference is gated post-acquisition,
// so this one name is best-effort and isolated here for easy reconciliation.
const char kPathMedicalRecords[] = "medical/records";

//------------------------------------------------------------------------------
// Local helpers
//------------------------------------------------------------------------------

// Percent-encode a query-string component (RFC 3986 unreserved pass through).
// Local copy, matching user/service.cpp, vitalera.cpp, and healthconnect.cpp.
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

class HumanApi : public VendorBase {
public:
    explicit HumanApi(VendorConfig cfg) : VendorBase(make_info(), std::move(cfg)) {}

    // Fetch `domain` over [start_iso, end_iso] from the Data API, as Human API's
    // JSON. The data is scoped to the user behind the access token, so `user_id`
    // is advisory: when non-empty it is sent as `human_id` to disambiguate, but
    // the access token is the real selector. Wellness domains hit their
    // documented per-resource endpoint; Clinical hits the (inferred) Medical
    // records path. Empty bounds omit the date filter. Returns the JSON verbatim.
    std::string fetch(const std::string& user_id,
                      DataDomain domain,
                      const std::string& start_iso,
                      const std::string& end_iso) override {
        require_token();

        const char* path = resource_path(domain);
        if (!path) {
            throw VendorError(info_.id + ": no Data API endpoint for domain '" +
                              to_string(domain) + "' (supported: activity, sleep, "
                              "heart_rate, glucose, body_metrics, clinical)");
        }

        std::string url = base_url() + path + "?";
        bool have_param = false;

        const std::string uid(user_id);
        if (!uid.empty()) {
            url += std::string("human_id=") + url_encode(uid);
            have_param = true;
        }
        if (!start_iso.empty()) {
            url += (have_param ? "&" : "");
            url += std::string(kSinceParam) + "=" + url_encode(std::string(start_iso));
            have_param = true;
        }
        if (!end_iso.empty()) {
            url += (have_param ? "&" : "");
            url += std::string(kUntilParam) + "=" + url_encode(std::string(end_iso));
            have_param = true;
        }
        if (!have_param) {
            url.pop_back();  // drop the dangling '?'
        }

        const std::vector<std::string> headers = {
            "Authorization: Bearer " + config().api_key,
            "Accept: application/json",
        };
        client::HttpResponse res = client::HttpClient().get(url, /*timeout_ms=*/30000, headers);
        if (res.status < 200 || res.status >= 300) {
            throw VendorError(info_.id + ": fetch " + path + " failed (HTTP " +
                              std::to_string(res.status) + "): " + res.body.substr(0, 300));
        }
        return res.body;
    }

private:
    // Map a DataDomain onto its Data API resource path. Wellness paths are
    // documented; Clinical -> kPathMedicalRecords is inferred (see constants).
    static const char* resource_path(DataDomain d) {
        switch (d) {
            case DataDomain::Activity:    return kPathActivities;
            case DataDomain::Sleep:       return kPathSleeps;
            case DataDomain::HeartRate:   return kPathHeartRate;
            case DataDomain::Glucose:     return kPathBloodGlucose;
            case DataDomain::BodyMetrics: return kPathBloodPressure;
            case DataDomain::Clinical:    return kPathMedicalRecords;
            default:                      return nullptr;
        }
    }

    // The Data API base, normalized to a trailing slash. Defaults to the
    // confirmed public host; an override (sandbox/region) is honored.
    std::string base_url() const {
        std::string b = config().base_url.empty() ? std::string(kDefaultBase) : config().base_url;
        if (!b.empty() && b.back() != '/') b.push_back('/');
        return b;
    }

    void require_token() const {
        if (config().api_key.empty()) {
            throw VendorError(info_.id + ": api_key is required — set "
                              "MIROBODY_VENDOR_HUMAN_API_API_KEY to a user accessToken "
                              "(obtained out of band by exchanging a Human Connect "
                              "sessionTokenObject at user.humanapi.co/v1/connect/tokens)");
        }
    }

    static VendorInfo make_info() {
        VendorInfo i;
        i.id                   = "human_api";
        i.display_name         = "Human API";
        i.positioning          = "Consumer-controlled health data aggregator";
        i.target_customers     = "Insurers, digital health, clinical research";
        i.data_source_coverage = "EHR from 90% of U.S. hospitals + 300+ wearables";
        i.integration_method   = "Unified RESTful API, consumer-authorization widget";
        i.compliance_summary   = "HIPAA, SOC 2";
        i.differentiator       = "Dual-track aggregation of clinical EHR and wearable data; major U.S. hospital coverage";
        i.docs_url             = "https://www.humanapi.co";
        i.region               = Region::US;
        i.open_source          = false;
        i.compliance           = {"HIPAA", "SOC2"};
        i.domains              = {DataDomain::Clinical, DataDomain::Activity};
        i.integrations         = {Integration::Rest};
        return i;
    }
};

}

std::unique_ptr<Vendor> make_human_api(const VendorConfig& cfg) {
    return std::unique_ptr<Vendor>(new HumanApi(cfg));
}

}}

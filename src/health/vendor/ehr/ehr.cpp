// Generic EHR client — one SMART-on-FHIR reader for every ONC-certified EHR
// (Epic, Oracle Health/Cerner, athenahealth, MEDITECH, Veradigm, …). Because the
// US ONC Cures Act forces these systems onto the SMART App Launch + FHIR R4
// standard, a single client parameterized by the tenant's base_url covers them
// all, rather than a near-identical file per vendor.
//
//   * base_url is REQUIRED and per-tenant: each hospital/clinic has its own FHIR
//     service base URL (discoverable from the public Service Base URL directories
//     — see directory.hpp). There is no single host to default to, so we refuse
//     rather than guess one.
//   * auth is a SMART-on-FHIR OAuth2 access token (config.api_key) sent as a
//     Bearer header; the authorize/token dance is per-tenant (discovered from
//     {base}/.well-known/smart-configuration) and runs out of band, which is why
//     authorize_url stays a stub rather than a half-built flow.
//   * fetch() issues standard FHIR R4 `Observation` searches — `patient`,
//     `category`, and the `date` range are published FHIR search parameters.
// See the SMART App Launch spec at https://hl7.org/fhir/smart-app-launch/.

#include "health/vendor/vendor.hpp"

#include "client/http_client.hpp"

#include <cctype>
#include <string>
#include <utility>

namespace mirobody { namespace vendor {
namespace {

// Percent-encode a query-string component (RFC 3986 unreserved pass through).
// Local copy, matching the other FHIR clients (healthconnect.cpp, vitalera.cpp).
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

class Ehr : public VendorBase {
public:
    explicit Ehr(VendorConfig cfg) : VendorBase(make_info(), std::move(cfg)) {}

    // Fetch `domain` for `user_id` (the FHIR Patient id) over [start_iso, end_iso]
    // as a FHIR R4 Observation Bundle. Domains map onto the standard Observation
    // `category` token; empty bounds omit the date filter. Returns the FHIR JSON.
    std::string fetch(const std::string& user_id,
                      DataDomain domain,
                      const std::string& start_iso,
                      const std::string& end_iso) override {
        const std::string base = require_base_url();
        require_token();

        if (user_id.empty()) {
            throw VendorError(info_.id + ": fetch requires a user_id (the FHIR Patient id)");
        }

        std::string url = base + "Observation?patient=" + url_encode(user_id);
        if (const char* category = fhir_category(domain)) {
            url += std::string("&category=") + category;
        }
        if (!start_iso.empty()) url += "&date=ge" + url_encode(start_iso);
        if (!end_iso.empty())   url += "&date=le" + url_encode(end_iso);

        const std::vector<std::string> headers = {
            "Authorization: Bearer " + config().api_key,
            "Accept: application/fhir+json",
        };
        client::HttpResponse res = client::HttpClient().get(url, /*timeout_ms=*/30000, headers);
        if (res.status < 200 || res.status >= 300) {
            throw VendorError(info_.id + ": fetch Observation failed (HTTP " +
                              std::to_string(res.status) + "): " + res.body.substr(0, 300));
        }
        return res.body;
    }

private:
    // Map a DataDomain onto the FHIR R4 Observation `category` code. Clinical spans
    // categories, so it is left unfiltered (nullptr) to return the patient's full
    // observation set. Sleep has no standard EHR Observation category, so reject it.
    static const char* fhir_category(DataDomain d) {
        switch (d) {
            case DataDomain::HeartRate:   return "vital-signs";
            case DataDomain::BodyMetrics: return "vital-signs";
            case DataDomain::Glucose:     return "laboratory";
            case DataDomain::Labs:        return "laboratory";
            case DataDomain::Activity:    return "activity";
            case DataDomain::Clinical:    return nullptr;
            default:
                throw VendorError(std::string("ehr: unsupported domain '") + to_string(d) +
                                  "' (EHR brokers: clinical, labs, glucose, heart_rate, body_metrics, activity)");
        }
    }

    // The tenant's FHIR service base URL, normalized to a trailing slash. Required:
    // each EHR tenant has its own endpoint, so there is nothing to default to.
    std::string require_base_url() const {
        std::string b = config().base_url;
        if (b.empty()) {
            throw VendorError(info_.id + ": base_url is required — set "
                              "MIROBODY_VENDOR_EHR_BASE_URL to the tenant's FHIR R4 service base "
                              "URL (from a Service Base URL directory; see ehr/directory.hpp)");
        }
        if (b.back() != '/') b.push_back('/');
        return b;
    }

    void require_token() const {
        if (config().api_key.empty()) {
            throw VendorError(info_.id + ": api_key is required — set "
                              "MIROBODY_VENDOR_EHR_API_KEY to a SMART-on-FHIR OAuth2 access token "
                              "(minted out of band against the tenant's authorization server)");
        }
    }

    static VendorInfo make_info() {
        VendorInfo i;
        i.id                   = "ehr";
        i.display_name         = "EHR (SMART on FHIR)";
        i.positioning          = "Direct EHR-system access via SMART on FHIR (Epic, Oracle Health/Cerner, athenahealth, …)";
        i.target_customers     = "Apps integrating directly with a patient's hospital/clinic EHR";
        i.data_source_coverage = "Any ONC-certified EHR exposing a SMART on FHIR R4 API";
        i.integration_method   = "SMART on FHIR (OAuth2 + FHIR R4); per-tenant base_url from a Service Base URL directory";
        i.compliance_summary   = "HIPAA; per-tenant SMART OAuth consent";
        i.differentiator       = "One client for every certified EHR — tenant base_url is discovered from public directories (see ehr/directory.hpp)";
        i.docs_url             = "https://hl7.org/fhir/smart-app-launch/";
        i.region               = Region::US;
        i.open_source          = false;
        i.compliance           = {"HIPAA"};
        i.domains              = {DataDomain::Clinical, DataDomain::Labs, DataDomain::Glucose,
                                  DataDomain::HeartRate, DataDomain::BodyMetrics, DataDomain::Activity};
        i.integrations         = {Integration::Rest, Integration::Fhir};
        return i;
    }
};

}

std::unique_ptr<Vendor> make_ehr(const VendorConfig& cfg) {
    return std::unique_ptr<Vendor>(new Ehr(cfg));
}

}}

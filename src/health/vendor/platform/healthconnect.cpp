// HealthConnect (CoPilot) — dual-track EHR + wearable interoperability platform.
// See README.md (supplementary competitor).
//
// HealthConnect CoPilot (by Mindbowser) is a B2B interoperability *accelerator*,
// not a self-serve public API: as of the 2026 report it publishes no developer
// reference — no base URL, no proprietary endpoint paths, no auth spec. Its pages
// confirm only the standards it speaks: OAuth2 bearer auth and FHIR (it stores
// metrics as "FHIR-compliant Observation resources" and bridges Epic/Cerner via
// HL7 V2 + FHIR). See https://www.mindbowser.com/healthconnect-copilot/.
//
// So rather than invent a proprietary contract, this client implements the
// STANDARD path the product exposes: FHIR R4 reads against the deployer's tenant.
//   * base_url is REQUIRED and supplied by the deployer (the HealthConnect/FHIR
//     tenant endpoint) — there is no public host to default to, so we refuse
//     rather than guess one.
//   * auth is an OAuth2 bearer access token (config.api_key); token acquisition
//     runs out of band against the tenant's authorization server, which is not
//     publicly documented.
//   * fetch() issues standard FHIR R4 `Observation` searches — `patient`,
//     `category`, and the `date` range are published FHIR search parameters, not
//     guesses.
// The dual-track real-time sync (its defining feature, i.e. FHIR Subscription /
// webhooks) and the consent flow stay stubs: those contracts are not documented,
// and fabricating them would be worse than an honest "not implemented".

#include "health/vendor/vendor.hpp"

#include "client/http_client.hpp"

#include <cctype>
#include <string>
#include <utility>

namespace mirobody { namespace vendor {
namespace {

//------------------------------------------------------------------------------
// Local helpers
//------------------------------------------------------------------------------

// Percent-encode a query-string component (RFC 3986 unreserved pass through).
// Local copy, matching user/service.cpp and vitalera.cpp.
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

class HealthConnect : public VendorBase {
public:
    explicit HealthConnect(VendorConfig cfg) : VendorBase(make_info(), std::move(cfg)) {}

    // Fetch `domain` for `user_id` over [start_iso, end_iso] as a FHIR R4
    // Observation Bundle. `user_id` is the FHIR Patient id (the `patient` search
    // parameter). Domains map onto the standard FHIR Observation `category`
    // token; empty bounds omit the date filter. Returns the FHIR JSON verbatim.
    std::string fetch(const std::string& user_id,
                      DataDomain domain,
                      const std::string& start_iso,
                      const std::string& end_iso) override {
        const std::string base = require_base_url();
        require_token();

        const std::string uid(user_id);
        if (uid.empty()) {
            throw VendorError(info_.id + ": fetch requires a user_id (the FHIR Patient id)");
        }

        std::string url = base + "Observation?patient=" + url_encode(uid);
        if (const char* category = fhir_category(domain)) {
            url += std::string("&category=") + category;
        }
        if (!start_iso.empty()) {
            url += "&date=ge" + url_encode(std::string(start_iso));
        }
        if (!end_iso.empty()) {
            url += "&date=le" + url_encode(std::string(end_iso));
        }

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
    // Map a DataDomain onto the FHIR R4 Observation `category` code. Returns
    // nullptr for Clinical, which spans categories — left unfiltered so the
    // search returns the patient's full observation set.
    static const char* fhir_category(DataDomain d) {
        switch (d) {
            case DataDomain::HeartRate:   return "vital-signs";
            case DataDomain::Activity:    return "activity";
            case DataDomain::Clinical:    return nullptr;
            default:
                throw VendorError(std::string("healthconnect: unsupported domain '") +
                                  to_string(d) + "' (configured domains: clinical, activity, heart_rate)");
        }
    }

    // The FHIR tenant endpoint, normalized to a trailing slash. Required: there
    // is no public HealthConnect host to default to.
    std::string require_base_url() const {
        std::string b = config().base_url;
        if (b.empty()) {
            throw VendorError(info_.id + ": base_url is required — set "
                              "MIROBODY_VENDOR_HEALTHCONNECT_BASE_URL to your FHIR tenant "
                              "endpoint (no public host to default to)");
        }
        if (b.back() != '/') b.push_back('/');
        return b;
    }

    void require_token() const {
        if (config().api_key.empty()) {
            throw VendorError(info_.id + ": api_key is required — set "
                              "MIROBODY_VENDOR_HEALTHCONNECT_API_KEY to an OAuth2 bearer "
                              "access token (acquired out of band from the tenant auth server)");
        }
    }

    static VendorInfo make_info() {
        VendorInfo i;
        i.id                   = "healthconnect";
        i.display_name         = "HealthConnect CoPilot";
        i.positioning          = "Dual-track EHR + wearable interoperability platform";
        i.target_customers     = "Healthcare providers, RPM developers, AI decision systems";
        i.data_source_coverage = "Epic/Cerner EHR + mainstream wearables";
        i.integration_method   = "HL7/FHIR standard API, real-time data sync";
        i.compliance_summary   = "HIPAA, medical-grade security";
        i.differentiator       = "Dual-track real-time sync purpose-built for RPM and AI clinical decision support";
        i.docs_url             = "https://www.mindbowser.com/healthconnect-copilot/";  // product home; no public API reference
        i.region               = Region::US;
        i.open_source          = false;
        i.compliance           = {"HIPAA"};
        i.domains              = {DataDomain::Clinical, DataDomain::Activity, DataDomain::HeartRate};
        i.integrations         = {Integration::Rest, Integration::Hl7, Integration::Fhir};
        return i;
    }
};

}

std::unique_ptr<Vendor> make_healthconnect(const VendorConfig& cfg) {
    return std::unique_ptr<Vendor>(new HealthConnect(cfg));
}

}}

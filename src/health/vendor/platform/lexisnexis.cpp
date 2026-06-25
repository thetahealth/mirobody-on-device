// LexisNexis Health Intelligence EHR — life-insurance underwriting EHR
// intelligence. See README.md (Segment C).
//
// LexisNexis® Health Intelligence (risk.lexisnexis.com, formerly Human API
// Health Intelligence) is a contract-gated enterprise underwriting platform —
// NOT a self-serve public API. As of the 2026 report it publishes no developer
// reference for this product: the product pages are marketing-only and route
// every integration question to sales ("Talk to an Expert"). There is no
// publicly documented base URL, no proprietary endpoint paths, no auth spec,
// and the pages do not even commit to a wire standard (FHIR/QHIN are described
// as capabilities, not as a documented contract). Access — base URL,
// credentials, the exact request shape — is handed to a carrier under a signed
// agreement. See https://risk.lexisnexis.com/products/health-intelligence-ehr.
//
// So rather than invent a proprietary contract, this client refuses to guess:
//   * base_url is REQUIRED and supplied by the deployer (the contracted
//     LexisNexis Health Intelligence endpoint) — there is no public host to
//     default to, so we refuse rather than fabricate one.
//   * auth is a bearer credential (config.api_key) sent in the Authorization
//     header; acquisition runs out of band against the contracted auth server,
//     which is not publicly documented.
//   * fetch() does NOT invent endpoint paths. It issues a deployer-driven
//     request: the caller supplies the resource path their contract documents
//     via start_iso (overloaded here as the path/query override — see fetch()),
//     and we attach the bearer and pass through the response verbatim. With no
//     path supplied there is nothing safe to call, so it errors with guidance.
// The consumer-mediated consent flow (QHIN authorize), the APS-handoff /
// Medical Insights extraction, webhooks, and revoke stay honest "not
// implemented" stubs: those contracts are gated and undocumented, and
// fabricating them would be worse than an honest stub.

#include "health/vendor/vendor.hpp"

#include "client/http_client.hpp"

#include <string>
#include <utility>
#include <vector>

namespace mirobody { namespace vendor {
namespace {

class LexisNexis : public VendorBase {
public:
    explicit LexisNexis(VendorConfig cfg) : VendorBase(make_info(), std::move(cfg)) {}

    // Fetch underwriting/EHR data from the deployer's contracted LexisNexis
    // Health Intelligence endpoint. Because LexisNexis publishes no public API
    // reference (see header comment), this client does NOT synthesize endpoint
    // paths from the DataDomain. Instead the request is deployer-driven:
    //   * base_url (required)        the contracted Health Intelligence host
    //   * api_key  (required)        bearer credential for that host
    //   * start_iso                  OVERLOADED as the resource path + query
    //                                exactly as the deployer's contract
    //                                documents it (e.g. "members/<id>/insights"
    //                                or "fhir/Observation?patient=...&date=ge...").
    //                                It is appended to base_url verbatim — the
    //                                caller owns the full request shape.
    //   * user_id, domain, end_iso   unused at the transport level; the path the
    //                                deployer supplies already encodes them. They
    //                                stay in the signature to satisfy the Vendor
    //                                interface and document intent.
    // Returns the contracted endpoint's response body verbatim. With no path
    // supplied there is nothing safe to call, so we error with guidance rather
    // than guess an endpoint.
    std::string fetch(const std::string& /*user_id*/,
                      DataDomain domain,
                      const std::string& start_iso,
                      const std::string& /*end_iso*/) override {
        const std::string base = require_base_url();
        require_token();

        const std::string path(start_iso);
        if (path.empty()) {
            throw VendorError(
                info_.id + ": fetch requires a contracted resource path for domain '" +
                to_string(domain) + "' — LexisNexis Health Intelligence publishes no public "
                "API reference, so no endpoint is assumed. Pass the path documented in your "
                "agreement via start_iso (e.g. \"fhir/Observation?patient=<id>&date=ge...\"); "
                "it is appended to MIROBODY_VENDOR_LEXISNEXIS_BASE_URL verbatim.");
        }

        // Append the deployer-supplied path verbatim. base is normalized to end
        // in '/'; if the caller's path is already absolute-ish ("/foo"), strip
        // the leading slash so we don't produce a double slash.
        std::string url = base;
        url += (path[0] == '/') ? path.substr(1) : path;

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
    // The contracted Health Intelligence endpoint, normalized to a trailing
    // slash. Required: there is no public LexisNexis host to default to.
    std::string require_base_url() const {
        std::string b = config().base_url;
        if (b.empty()) {
            throw VendorError(info_.id + ": base_url is required — set "
                              "MIROBODY_VENDOR_LEXISNEXIS_BASE_URL to your contracted "
                              "LexisNexis Health Intelligence endpoint (no public host to "
                              "default to; this is a sales-gated enterprise platform)");
        }
        if (b.back() != '/') b.push_back('/');
        return b;
    }

    void require_token() const {
        if (config().api_key.empty()) {
            throw VendorError(info_.id + ": api_key is required — set "
                              "MIROBODY_VENDOR_LEXISNEXIS_API_KEY to the bearer credential "
                              "issued under your LexisNexis agreement (acquired out of band)");
        }
    }

    static VendorInfo make_info() {
        VendorInfo i;
        i.id                   = "lexisnexis";
        i.display_name         = "LexisNexis EHR";
        i.positioning          = "Life-insurance underwriting EHR intelligence platform";
        i.target_customers     = "Life insurance carriers, distributors";
        i.data_source_coverage = "30,000+ U.S. data sources; EHR, lab, BMI, behavioral data";
        i.integration_method   = "API access with QHINs consumer-mediated consent";
        i.compliance_summary   = "HIPAA authorized network";
        i.differentiator       = "Medical Insights underwriting-attribute extraction; automatic APS-statement handoff";
        i.docs_url             = "https://risk.lexisnexis.com";
        i.region               = Region::US;
        i.open_source          = false;
        i.compliance           = {"HIPAA", "QHIN"};
        i.domains              = {DataDomain::Clinical, DataDomain::Labs, DataDomain::BodyMetrics};
        i.integrations         = {Integration::Rest};
        return i;
    }
};

}

std::unique_ptr<Vendor> make_lexisnexis(const VendorConfig& cfg) {
    return std::unique_ptr<Vendor>(new LexisNexis(cfg));
}

}}

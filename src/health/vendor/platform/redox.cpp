// Redox — healthcare interoperability platform. See README.md (Segment C).
//
// Implemented against the public reference at https://docs.redoxengine.com — a
// deep-EHR interoperability platform that exposes a FHIR R4 API plus proprietary
// real-time message streams over HL7/FHIR. This client implements the STANDARD
// path Redox publishes: FHIR R4 reads against the deployer's destination, gated
// behind the OAuth2 bearer Redox issues.
//
// CONFIRMED from the public docs:
//   * auth token endpoint POST https://api.redoxengine.com/v2/auth/token, body
//                         grant_type=client_credentials &
//                         client_assertion_type=urn:ietf:params:oauth:client-assertion-type:jwt-bearer &
//                         client_assertion=<signed JWT>; response JSON carries the
//                         bearer in "access_token" (+ token_type / expires_in,
//                         5-minute lifetime). This is SMART Backend Services Auth.
//   * bearer use          Authorization: Bearer <access_token> on every API call.
//   * FHIR R4 base        https://api.redoxengine.com/fhir/R4/{destination-slug}/{environment}
//                         e.g. .../redox-fhir-sandbox/Development/Patient/123
//   * FHIR search         patient= and the date= range (ge/le prefixes) are the
//                         FHIR R4 standard search parameters, not a guess.
//
// AUTH HANDLING — the Redox token flow requires a JWT signed with the partner's
// PRIVATE KEY (referenced by `kid`, published via a JWKS URL). VendorConfig holds
// no private key or signing material, so minting that assertion cannot be done
// here. Following vitalera/healthconnect's honest approach, we accept a
// pre-obtained bearer access token via config.api_key; acquiring it (signing the
// client assertion and POSTing it to /v2/auth/token) runs out of band. We do NOT
// attempt the token POST with the plain client_id/client_secret in VendorConfig:
// Redox does not accept a client_secret grant, so that would only ever fail.
//
// BASE URL — there is no usable public default: the FHIR base embeds the
// deployer's destination-slug and environment, which only the deployer knows. So
// base_url is REQUIRED (the full FHIR R4 base, through the environment segment),
// and we refuse rather than fabricate one.
//
// The proprietary real-time message-stream API (Redox's defining operation) and
// the consent flow stay inherited stubs: those contracts are not cleanly
// expressible from VendorConfig, and fabricating them would be worse than an
// honest "not implemented".

#include "health/vendor/vendor.hpp"

#include "client/http_client.hpp"

#include <cctype>
#include <string>
#include <utility>
#include <vector>

namespace mirobody { namespace vendor {
namespace {

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

class Redox : public VendorBase {
public:
    explicit Redox(VendorConfig cfg) : VendorBase(make_info(), std::move(cfg)) {}

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
            case DataDomain::Clinical: return nullptr;
            default:
                throw VendorError(std::string("redox: unsupported domain '") +
                                  to_string(d) + "' (Redox brokers clinical EHR data: domain 'clinical')");
        }
    }

    // The FHIR R4 base, normalized to a trailing slash. Required: the public base
    // embeds the deployer's destination-slug and environment
    // (https://api.redoxengine.com/fhir/R4/{destination-slug}/{environment}/),
    // so there is no host to default to — we refuse rather than guess one.
    std::string require_base_url() const {
        std::string b = config().base_url;
        if (b.empty()) {
            throw VendorError(info_.id + ": base_url is required — set "
                              "MIROBODY_VENDOR_REDOX_BASE_URL to your FHIR R4 base "
                              "(https://api.redoxengine.com/fhir/R4/<destination-slug>/<environment>/); "
                              "no public host to default to");
        }
        if (b.back() != '/') b.push_back('/');
        return b;
    }

    // Require a bearer access token. The token is obtained out of band by signing
    // a JWT client assertion with the partner private key and POSTing it to
    // https://api.redoxengine.com/v2/auth/token — material VendorConfig does not
    // carry — so we accept the resulting access_token via api_key.
    void require_token() const {
        if (config().api_key.empty()) {
            throw VendorError(info_.id + ": api_key is required — set "
                              "MIROBODY_VENDOR_REDOX_API_KEY to an OAuth2 bearer access token "
                              "(minted out of band via a signed JWT client assertion to "
                              "https://api.redoxengine.com/v2/auth/token; tokens last ~5 minutes)");
        }
    }

    static VendorInfo make_info() {
        VendorInfo i;
        i.id                   = "redox";
        i.display_name         = "Redox";
        i.positioning          = "Healthcare interoperability platform";
        i.target_customers     = "Healthcare IT architects, integration engineers, hospitals";
        i.data_source_coverage = "Deep EHR systems (HL7, FHIR standard data)";
        i.integration_method   = "API-driven, real-time message streams, standardized data exchange";
        i.compliance_summary   = "HIPAA, SOC 2 Type II, HITRUST";
        i.differentiator       = "High-speed, high-reliability data exchange between EHR systems and cloud apps";
        i.docs_url             = "https://www.redoxengine.com";
        i.region               = Region::US;
        i.open_source          = false;
        i.compliance           = {"HIPAA", "SOC2_TYPE_II", "HITRUST"};
        i.domains              = {DataDomain::Clinical};
        i.integrations         = {Integration::Rest, Integration::Hl7, Integration::Fhir};
        return i;
    }
};

}

std::unique_ptr<Vendor> make_redox(const VendorConfig& cfg) {
    return std::unique_ptr<Vendor>(new Redox(cfg));
}

}}

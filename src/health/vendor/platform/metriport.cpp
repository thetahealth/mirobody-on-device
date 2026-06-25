// Metriport — open-source medical data interoperability platform. See README.md
// (Strategic Option A).
//
// Implemented against the public developer docs at https://docs.metriport.com
// and the open-source TypeScript SDK (github.com/metriport/metriport,
// packages/api-sdk). Metriport runs two products behind one host and one API
// key, so this client routes by domain:
//   * Clinical            -> Medical API: consolidated FHIR R4 data for a patient.
//   * wearable domains     -> Devices API: per-day metric reads for a connected user.
//
// CONFIRMED from packages/api-sdk/src/shared.ts and the medical/devices clients:
//   * production base       https://api.metriport.com           (BASE_ADDRESS)
//   * sandbox base          https://api.sandbox.metriport.com    (BASE_ADDRESS_SANDBOX)
//                           selected via base_url override (Metriport is also
//                           self-hostable, so base_url is meaningful either way).
//   * auth header           x-api-key: <key>                     (API_KEY_HEADER)
//   * Medical API path      BASE_PATH = "/medical/v1"
//       GET /medical/v1/patient/{id}/consolidated
//           ?resources=<csv>&dateFrom=YYYY-MM-DD&dateTo=YYYY-MM-DD
//       (the SDK's getPatientConsolidated() — returns the patient's consolidated
//        FHIR Bundle directly. The newer async POST .../consolidated/query +
//        webhook flow is left as the inherited stub; our fetch() contract is
//        synchronous request/response.)
//   * Devices API paths     GET /activity | /sleep | /biometrics | /body |
//                           /nutrition  ?userId=<id>&date=YYYY-MM-DD
//       (the SDK's getActivityData/getSleepData/getBiometricsData/getBodyData/
//        getNutritionData — all take userId + a single date in YYYY-MM-DD.)
//
// NOTHING here is inferred: every path, parameter name, header, and host below is
// taken verbatim from the open-source SDK. Two contract mismatches are handled
// honestly rather than papered over:
//   * Devices reads are single-day; our fetch() takes a [start,end] range. We
//     pass start_iso as the `date` and ignore end_iso (documented at the call
//     site) rather than invent an undocumented range parameter.
//   * Metriport dates are YYYY-MM-DD; ISO-8601 timestamps are truncated to their
//     date component before being sent.
// authorize_url / list_providers / handle_webhook / revoke stay inherited stubs —
// those map to the Connect-widget token, connected-providers, and webhook flows,
// which don't fit these signatures cleanly and aren't required here.

#include "health/vendor/vendor.hpp"

#include "client/http_client.hpp"

#include <cctype>
#include <string>
#include <utility>
#include <vector>

namespace mirobody { namespace vendor {
namespace {

//------------------------------------------------------------------------------
// Confirmed constants (packages/api-sdk/src/shared.ts + client paths).
//------------------------------------------------------------------------------

const char kDefaultBase[]  = "https://api.metriport.com";
const char kApiKeyHeader[] = "x-api-key";

// Medical API base path (BASE_PATH in the SDK's medical client).
const char kMedicalBase[] = "/medical/v1";

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

// Metriport's date params are YYYY-MM-DD. ISO-8601 inputs may carry a time
// component ("2026-06-05T12:00:00Z"); keep only the date prefix. A bare date
// passes through unchanged.
std::string to_date_only(const std::string& iso) {
    std::string s(iso);
    const std::string::size_type t = s.find('T');
    if (t != std::string::npos) s.resize(t);
    return s;
}

//------------------------------------------------------------------------------

class Metriport : public VendorBase {
public:
    explicit Metriport(VendorConfig cfg) : VendorBase(make_info(), std::move(cfg)) {}

    // Fetch `domain` for the user over [start_iso, end_iso]. Clinical routes to
    // the Medical API consolidated FHIR endpoint (full date range honored); every
    // other domain routes to the Devices API per-day endpoint, which accepts a
    // single `date` — we use start_iso for it and ignore end_iso. Returns the
    // response JSON verbatim. Empty bounds simply omit the corresponding param.
    std::string fetch(const std::string& user_id,
                      DataDomain domain,
                      const std::string& start_iso,
                      const std::string& end_iso) override {
        require_configured();
        const std::string uid(user_id);
        if (uid.empty()) {
            throw VendorError(info_.id + ": fetch requires a user_id "
                              "(the Metriport patient id for clinical, or the "
                              "Metriport user id for wearable domains)");
        }

        if (domain == DataDomain::Clinical) {
            return fetch_consolidated(uid, start_iso, end_iso);
        }
        return fetch_device_metric(uid, domain, start_iso);
    }

private:
    //--------------------------------------------------------------------------
    // Medical API — consolidated FHIR R4 for a patient.
    //   GET /medical/v1/patient/{id}/consolidated
    //       ?resources=<csv>&dateFrom=YYYY-MM-DD&dateTo=YYYY-MM-DD
    //--------------------------------------------------------------------------
    std::string fetch_consolidated(const std::string& uid,
                                   const std::string& start_iso,
                                   const std::string& end_iso) {
        std::string url = base_url() + kMedicalBase +
                          "/patient/" + url_encode(uid) + "/consolidated";
        char sep = '?';
        if (!start_iso.empty()) {
            url += sep; sep = '&';
            url += "dateFrom=" + url_encode(to_date_only(start_iso));
        }
        if (!end_iso.empty()) {
            url += sep; sep = '&';
            url += "dateTo=" + url_encode(to_date_only(end_iso));
        }
        // `resources` is left unset: omitting it returns all FHIR resource types,
        // which is what a domain-agnostic Clinical fetch wants.
        return get_json(url, "fetch patient/consolidated");
    }

    //--------------------------------------------------------------------------
    // Devices API — per-day metric reads for a connected user.
    //   GET /<metric>?userId=<id>&date=YYYY-MM-DD
    //--------------------------------------------------------------------------
    std::string fetch_device_metric(const std::string& uid,
                                     DataDomain domain,
                                     const std::string& start_iso) {
        const char* endpoint = device_endpoint(domain);
        if (!endpoint) {
            throw VendorError(info_.id + ": no Devices API endpoint for domain '" +
                              to_string(domain) + "'");
        }
        std::string url = base_url() + endpoint + "?userId=" + url_encode(uid);
        // The Devices endpoints require a single `date`. fetch() gives a range;
        // we send start_iso as the day. When start is empty there is no date to
        // send, so the request is left without one and Metriport will reject it —
        // surfaced as a VendorError, which is the honest outcome.
        if (!start_iso.empty()) {
            url += "&date=" + url_encode(to_date_only(start_iso));
        }
        return get_json(url, std::string("fetch ") + endpoint);
    }

    // Map a DataDomain onto its Devices API endpoint. Domains Metriport does not
    // expose a wearable read for return nullptr.
    static const char* device_endpoint(DataDomain d) {
        switch (d) {
            case DataDomain::Activity:    return "/activity";
            case DataDomain::Sleep:       return "/sleep";
            case DataDomain::HeartRate:   return "/biometrics";  // HR lives under biometrics
            case DataDomain::BodyMetrics: return "/body";
            case DataDomain::Nutrition:   return "/nutrition";
            // Glucose/Labs ride under biometrics for some providers, but there is
            // no dedicated documented endpoint, so leave them unmapped.
            default:                      return nullptr;
        }
    }

    //--------------------------------------------------------------------------
    // Transport
    //--------------------------------------------------------------------------
    std::string get_json(const std::string& url, const std::string& op) {
        const std::vector<std::string> headers = {
            std::string(kApiKeyHeader) + ": " + config().api_key,
            "Accept: application/json",
        };
        client::HttpResponse res = client::HttpClient().get(url, /*timeout_ms=*/30000, headers);
        if (res.status < 200 || res.status >= 300) {
            // status <= 0 is a transport failure; curl puts the reason in body.
            throw VendorError(info_.id + ": " + op + " failed (HTTP " +
                              std::to_string(res.status) + "): " + res.body.substr(0, 300));
        }
        return res.body;
    }

    //--------------------------------------------------------------------------
    // Config
    //--------------------------------------------------------------------------
    void require_configured() const {
        if (config().api_key.empty()) {
            throw VendorError(info_.id + ": not configured — set "
                              "MIROBODY_VENDOR_METRIPORT_API_KEY (and optionally "
                              "MIROBODY_VENDOR_METRIPORT_BASE_URL to target the "
                              "sandbox https://api.sandbox.metriport.com or a "
                              "self-hosted host)");
        }
    }

    // API host, normalized without a trailing slash (paths below all start '/').
    std::string base_url() const {
        std::string b = config().base_url.empty() ? std::string(kDefaultBase)
                                                   : config().base_url;
        while (!b.empty() && b.back() == '/') b.pop_back();
        return b;
    }

    static VendorInfo make_info() {
        VendorInfo i;
        i.id                   = "metriport";
        i.display_name         = "Metriport";
        i.positioning          = "Open-source medical data interoperability platform";
        i.target_customers     = "Digital health startups, health-system developers";
        i.data_source_coverage = "Major U.S. healthcare IT systems (EHR clinical data)";
        i.integration_method   = "Modern API, developer dashboard, FHIR R4 format";
        i.compliance_summary   = "HIPAA, open-source self-hosted compliance";
        i.differentiator       = "Open-source; standardizes, consolidates, and de-duplicates clinical records into FHIR R4";
        i.docs_url             = "https://www.metriport.com";
        i.region               = Region::US;
        i.open_source          = true;
        i.compliance           = {"HIPAA"};
        i.domains              = {DataDomain::Clinical};
        i.integrations         = {Integration::Rest, Integration::Fhir};
        return i;
    }
};

}

std::unique_ptr<Vendor> make_metriport(const VendorConfig& cfg) {
    return std::unique_ptr<Vendor>(new Metriport(cfg));
}

}}

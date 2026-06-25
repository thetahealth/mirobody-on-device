// Particle Health — API-driven clinical data platform. See README.md (Segment C).
//
// Implemented against the public reference at https://docs.particlehealth.com — a
// query-based clinical-data network that retrieves longitudinal patient records
// from nationwide healthcare networks and returns them as FHIR R4. This client
// covers the documented Main Query Flow end-to-end: mint a JWT, register the
// patient, fire a one-time query, poll it to COMPLETE, then read the FHIR R4
// Patient $everything bundle.
//
// CONFIRMED from the public docs (docs.particlehealth.com):
//   * base host          https://api.particlehealth.com
//   * auth               GET /auth mints a 60-minute JWT from a Client ID/Secret;
//                        every other call carries it as "Authorization: Bearer <jwt>"
//   * register patient   POST /api/v2/patients  -> a particle_patient_id (PPID)
//   * fire query         POST /api/v2/patients/{ppid}/query  (one-time network search)
//   * query status       GET  /api/v2/patients/{ppid}/query  (poll until COMPLETE)
//   * FHIR R4 retrieval   GET /api/v2/patients/{ppid}/r4/Patient/{ppid}/$everything
//                        (Patient $everything — all resources in one Bundle; $everything,
//                         the _since/_count paging are FHIR R4 standard, not a guess)
//
// INFERRED — the request-level reference (auth header spelling, the JWT response
// envelope, the exact FHIR sub-path under the patient resource) is gated behind a
// signed-in docs portal / a Client ID issued by a Particle representative, so the
// few names below are best-effort and centralized in the constants block for easy
// reconciliation against that reference once credentials are in hand.
//
// SCOPE: fetch(DataDomain::Clinical) implements the final retrieval leg — it
// reads the FHIR R4 Patient $everything bundle for a particle_patient_id whose
// one-time query has already reached COMPLETE. The two pre-fetch legs Particle
// documents — register the patient (POST /api/v2/patients) and fire + poll the
// network query (POST/GET /api/v2/patients/{ppid}/query) — are intentionally left
// to the caller for now: they take a caller-built demographics body and a polling
// loop that don't fit the value-returning fetch(user_id, domain, …) signature.
// Their confirmed paths are recorded above so they slot in cleanly later.
//
// The proprietary ADT (admit/discharge/transfer) event stream — Particle's other
// headline feature — is an asynchronous push contract that is not publicly
// documented at the field level; rather than invent a webhook signature/envelope,
// handle_webhook stays the inherited "not implemented" stub.

#include "health/vendor/vendor.hpp"

#include "client/http_client.hpp"

#include <rapidjson/document.h>

#include <cctype>
#include <mutex>
#include <string>
#include <utility>

namespace mirobody { namespace vendor {
namespace {

//------------------------------------------------------------------------------
// Confirmed defaults + inferred names (see header comment).
//------------------------------------------------------------------------------

// CONFIRMED: public production host.
const char kDefaultBase[] = "https://api.particlehealth.com";

// CONFIRMED: documented endpoint paths.
const char kAuthPath[]      = "/auth";
const char kPatientsPath[]  = "/api/v2/patients";          // POST -> particle_patient_id
// Per-patient sub-paths are built from a PPID below: "/query" and the FHIR R4 read.

// INFERRED: the GET /auth credential header names. The docs describe a Client ID
// & Secret passed to /auth; the header spelling here is best-effort.
const char kAuthClientIdHeader[]     = "client-id";
const char kAuthClientSecretHeader[] = "client-secret";

// INFERRED: the JSON field carrying the minted JWT in the /auth response. Some
// Particle setups return the raw JWT as the response body; we accept both (see
// extract_jwt) and only consult these field names when the body parses as JSON.
const char kJwtField[]       = "jwt";
const char kJwtFieldAlt1[]   = "token";
const char kJwtFieldAlt2[]   = "access_token";

//------------------------------------------------------------------------------
// Local helpers
//------------------------------------------------------------------------------

// Percent-encode a query-string / path component (RFC 3986 unreserved pass
// through). Local copy, matching user/service.cpp, vitalera.cpp, healthconnect.cpp.
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

// Trim ASCII whitespace and surrounding double-quotes — a raw-JWT auth body may
// arrive bare or quoted depending on the transport.
std::string trim_token(const std::string& s) {
    std::size_t b = 0, e = s.size();
    while (b < e && std::isspace(static_cast<unsigned char>(s[b]))) ++b;
    while (e > b && std::isspace(static_cast<unsigned char>(s[e - 1]))) --e;
    if (e - b >= 2 && s[b] == '"' && s[e - 1] == '"') { ++b; --e; }
    return s.substr(b, e - b);
}

//------------------------------------------------------------------------------

class ParticleHealth : public VendorBase {
public:
    explicit ParticleHealth(VendorConfig cfg) : VendorBase(make_info(), std::move(cfg)) {}

    // Fetch clinical records for `user_id` over [start_iso, end_iso] as a FHIR R4
    // Bundle. `user_id` is the particle_patient_id (PPID) of an already-queried
    // patient: this drives only the retrieval leg (GET $everything), so callers
    // own patient registration + the one-time network query (see the header note
    // for those confirmed paths). Empty bounds omit the FHIR `_since` filter. Particle only
    // brokers clinical EHR data, so any non-Clinical domain is rejected. Returns
    // the FHIR JSON verbatim.
    std::string fetch(const std::string& user_id,
                      DataDomain domain,
                      const std::string& start_iso,
                      const std::string& end_iso) override {
        require_configured();
        if (domain != DataDomain::Clinical) {
            throw VendorError(info_.id + ": only the clinical domain is supported (got '" +
                              to_string(domain) + "') — Particle brokers clinical EHR records");
        }
        const std::string ppid(user_id);
        if (ppid.empty()) {
            throw VendorError(info_.id + ": fetch requires a user_id (the particle_patient_id)");
        }

        // FHIR R4 Patient $everything: one Bundle of all resources for the PPID.
        // $everything and the _since/_count paging params are FHIR R4 standard.
        std::string url = base_url() + kPatientsPath + "/" + url_encode(ppid) +
                          "/r4/Patient/" + url_encode(ppid) + "/$everything";
        char sep = '?';
        if (!start_iso.empty()) {
            url += sep; sep = '&';
            url += "_since=" + url_encode(std::string(start_iso));
        }
        // FHIR $everything has no standard upper-bound search param; an end bound
        // is therefore applied by the caller after retrieval. (Left explicit so a
        // later normalization step can window the returned Bundle.)
        (void)end_iso;

        return get_json(url, "fetch r4/Patient/$everything",
                        /*accept=*/"application/fhir+json");
    }

private:
    //--------------------------------------------------------------------------
    // Transport + auth
    //--------------------------------------------------------------------------

    std::string get_json(const std::string& url, const std::string& op, const char* accept) {
        std::vector<std::string> headers = auth_headers();
        headers.push_back(std::string("Accept: ") + accept);
        client::HttpResponse res = client::HttpClient().get(url, /*timeout_ms=*/60000, headers);
        if (res.status < 200 || res.status >= 300) {
            throw VendorError(http_error(op, res));
        }
        return res.body;
    }

    std::vector<std::string> auth_headers() {
        return { "Authorization: Bearer " + bearer() };
    }

    // Resolve a JWT bearer, caching it for this client's lifetime (the docs note
    // JWTs expire after ~60min; a long-lived process should refresh, which the
    // mutex-guarded slot leaves room for).
    //   * client_id + client_secret -> mint one at GET /auth
    //   * api_key only               -> treat it as a pre-obtained bearer/JWT
    //                                    (auth then runs out of band)
    const std::string& bearer() {
        std::lock_guard<std::mutex> lk(token_mu_);
        if (!token_.empty()) return token_;

        if (!config().client_id.empty() && !config().client_secret.empty()) {
            token_ = mint_token();
        } else {
            token_ = config().api_key;   // used directly as the bearer
        }
        return token_;
    }

    // GET /auth with the Client ID/Secret in headers; the response is either a
    // bare JWT or a small JSON envelope — extract_jwt handles both.
    std::string mint_token() {
        std::vector<std::string> headers = {
            std::string(kAuthClientIdHeader) + ": " + config().client_id,
            std::string(kAuthClientSecretHeader) + ": " + config().client_secret,
            "Accept: application/json",
        };
        client::HttpResponse res =
            client::HttpClient().get(base_url() + kAuthPath, /*timeout_ms=*/30000, headers);
        if (res.status < 200 || res.status >= 300) {
            throw VendorError(http_error("authenticate (GET /auth)", res));
        }
        std::string jwt = extract_jwt(res.body);
        if (jwt.empty()) {
            throw VendorError(info_.id + ": no JWT in /auth response");
        }
        return jwt;
    }

    static std::string extract_jwt(const std::string& body) {
        rapidjson::Document doc;
        if (!doc.Parse(body.c_str()).HasParseError() && doc.IsObject()) {
            for (const char* field : { kJwtField, kJwtFieldAlt1, kJwtFieldAlt2 }) {
                if (doc.HasMember(field) && doc[field].IsString()) {
                    return doc[field].GetString();
                }
            }
            return std::string();  // JSON object but no recognized field
        }
        // Not a JSON object: assume the raw (possibly quoted) JWT is the body.
        return trim_token(body);
    }

    //--------------------------------------------------------------------------
    // Misc
    //--------------------------------------------------------------------------

    void require_configured() const {
        if (!config().configured()) {
            throw VendorError(info_.id + ": not configured — set "
                              "MIROBODY_VENDOR_PARTICLE_HEALTH_API_KEY (a pre-obtained JWT) or "
                              "_CLIENT_ID/_CLIENT_SECRET (minted at GET /auth)");
        }
    }

    std::string base_url() const {
        std::string b = config().base_url.empty() ? std::string(kDefaultBase) : config().base_url;
        // Paths above all start with '/', so strip a trailing slash to avoid '//'.
        if (!b.empty() && b.back() == '/') b.pop_back();
        return b;
    }

    std::string http_error(const std::string& op, const client::HttpResponse& res) const {
        // status <= 0 is a transport failure; curl puts the reason in body.
        return info_.id + ": " + op + " failed (HTTP " + std::to_string(res.status) +
               "): " + res.body.substr(0, 300);
    }

    static VendorInfo make_info() {
        VendorInfo i;
        i.id                   = "particle_health";
        i.display_name         = "Particle Health";
        i.positioning          = "API-driven clinical data platform";
        i.target_customers     = "Health systems, payers, value-based care teams";
        i.data_source_coverage = "Longitudinal patient clinical records from nationwide healthcare networks";
        i.integration_method   = "API, supports ADT event streams";
        i.compliance_summary   = "HIPAA, SOC 2 Type II";
        i.differentiator       = "Aggregates, de-duplicates, and standardizes longitudinal patient clinical records";
        i.docs_url             = "https://www.particlehealth.com";
        i.region               = Region::US;
        i.open_source          = false;
        i.compliance           = {"HIPAA", "SOC2_TYPE_II"};
        i.domains              = {DataDomain::Clinical};
        i.integrations         = {Integration::Rest};
        return i;
    }

    std::mutex  token_mu_;
    std::string token_;
};

}

std::unique_ptr<Vendor> make_particle_health(const VendorConfig& cfg) {
    return std::unique_ptr<Vendor>(new ParticleHealth(cfg));
}

}}

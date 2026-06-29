// Vitalera — next-generation health data API platform. See README.md (Segment B).
//
// Implemented against the public reference at https://docs-v2.vitalera.io — a
// FHIR-R5 health-data platform (500+ wearables + medical devices, ECG, blood
// glucose) certified as medical software (SaMD). This client covers the two
// operations the platform leads with: the native device-fetch path (per-metric
// REST list endpoints) and FHIR output (FHIR R5 Observation), plus the JWT auth
// it gates every call behind, and disconnect via the auth deactivate endpoint.
//
// CONFIRMED from the public docs (docs-v2.vitalera.io/platform-api/authentication):
//   * base URL            https://api.vitalera.io/api/
//   * auth                OAuth 2.0 + JWT Bearer in the Authorization header.
//                         Tokens minted at POST /api/auth/tokens/ with a body of
//                         { grant_type: "client_credentials", client_id,
//                         client_secret }; the response carries the token in the
//                         "access_token" field. Tokens last 3600s; refresh at
//                         /api/auth/tokens/refresh/, validate at
//                         /api/auth/tokens/validate/.
//   * device-fetch path   GET /<metric>/ list endpoints (heart-rate, blood-glucose,
//                         blood-pressure, oxygen-saturation, temperature, step-count,
//                         calories, workouts), DRF pagination (count/next/previous)
//   * FHIR                FHIR R5 resources (Patient, Observation, …)
//   * list_providers      GET /api/connected-accounts/services/ — the provider
//                         catalogue (entitlement-filtered to the caller's org).
//                         Method + path are public; the response shape is in the
//                         gated reference, so we return the JSON verbatim.
//   * handle_webhook      outbound webhooks POST a JSON envelope
//                         { event_type, timestamp, organization_id, data }; the
//                         signature is in the `x-webhook-signature` header as
//                         lowercase-hex HMAC-SHA256 over the RAW request body,
//                         keyed by the webhook ENDPOINT secret. The body's
//                         `timestamp` is NOT part of the signed string. This whole
//                         scheme is public (docs-v2.vitalera.io/webhooks/overview).
//                         The endpoint secret is a DIFFERENT credential from the
//                         OAuth client_secret; since VendorConfig has one slot, it
//                         rides on config.client_secret — so a deployment that also
//                         mints tokens via client-credentials should set api_key to
//                         a pre-issued bearer (the fetch path falls back to it when
//                         client_id is empty), leaving client_secret for the webhook.
//
// authorize_url stays a stub: the connect flow is documented at the path level
// (POST /api/connected-accounts/connect-session/ then a hosted GET
// /api/connected-accounts/connect/?token=…, or per-provider POST
// /api/connected-accounts/{service_id}/oauth/initiate/), but whether it accepts a
// redirect_uri/state and what URL field it returns live in the gated field-level
// reference — so a faithful authorize_url(redirect_uri, state) can't be built from
// public docs yet.
//
// INFERRED — the field-level reference is gated (Vitalera issues it after sign-up
// via info@vitalera.io), so these names are best-effort and centralized in the
// constants below for easy reconciliation against that reference:
//   * the native list endpoints' patient + date-range query parameters,
//   * the FHIR base path segment ("fhir/"),
//   * the token-deactivation path used by revoke (the auth family is confirmed
//     to expose a deactivate op, but its exact route is not public).
// The FHIR *search* semantics (patient=, date=ge.../date=le...) are the R5
// standard, not a guess.

#include "health/vendor/vendor.hpp"

#include "client/http_client.hpp"
#include "storage/sign.hpp"   // hmac_sha256 / hex_encode

#include <rapidjson/document.h>

#include <array>
#include <cctype>
#include <cstddef>
#include <mutex>
#include <string>
#include <utility>

namespace mirobody { namespace vendor {
namespace {

//------------------------------------------------------------------------------
// Confirmed defaults + inferred names (see header comment).
//------------------------------------------------------------------------------

const char kDefaultBase[] = "https://api.vitalera.io/api/";

// Confirmed: the JWT mint endpoint (relative to kDefaultBase) and the
// client-credentials request-body fields posted to it.
const char kAuthPath[]              = "auth/tokens/";
const char kAuthGrantType[]         = "client_credentials";
const char kAuthClientIdField[]     = "client_id";
const char kAuthClientSecretField[] = "client_secret";

// Inferred: native list-endpoint query params. `patient` selects the monitored
// user; the date range filters the returned window. (FHIR search below uses the
// standard `patient`/`date` params instead.)
const char kPatientParam[] = "patient";
const char kStartParam[]   = "start";
const char kEndParam[]     = "end";

// Inferred: FHIR base path segment under kDefaultBase.
const char kFhirSegment[] = "fhir/";

//------------------------------------------------------------------------------
// Local helpers
//------------------------------------------------------------------------------

// Percent-encode a query-string component (RFC 3986 unreserved pass through).
// ISO-8601 bounds and opaque user ids both carry reserved characters, so every
// interpolated value goes through this. Local copy, matching user/service.cpp.
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

// JSON-string-escape a value for the small auth body we build by hand (avoids
// pulling in a Writer for two fields).
std::string json_escape(const std::string& s) {
    std::string out;
    out.reserve(s.size() + 2);
    for (char c : s) {
        switch (c) {
            case '"':  out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n";  break;
            case '\r': out += "\\r";  break;
            case '\t': out += "\\t";  break;
            default:   out.push_back(c);
        }
    }
    return out;
}

// Constant-time compare of two strings (length itself is not secret). Used to
// compare webhook signatures without leaking match position via timing.
bool ct_equal(const std::string& a, const std::string& b) {
    if (a.size() != b.size()) return false;
    unsigned char diff = 0;
    for (std::size_t i = 0; i < a.size(); ++i) {
        diff |= static_cast<unsigned char>(a[i]) ^ static_cast<unsigned char>(b[i]);
    }
    return diff == 0;
}

// Find a header value in a raw "Name: value\r\n..." block, matching `name`
// case-insensitively (HTTP header names are case-insensitive). Returns the
// trimmed value, or "" if absent.
std::string header_value(const std::string& raw, const char* name) {
    std::string lname(name);
    for (char& c : lname) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));

    std::size_t pos = 0;
    while (pos < raw.size()) {
        std::size_t eol = raw.find('\n', pos);
        std::string line = raw.substr(pos, eol == std::string::npos ? std::string::npos
                                                                     : eol - pos);
        pos = (eol == std::string::npos) ? raw.size() : eol + 1;
        if (!line.empty() && line.back() == '\r') line.pop_back();

        std::size_t colon = line.find(':');
        if (colon == std::string::npos) continue;

        std::string key = line.substr(0, colon);
        std::size_t ks = key.find_first_not_of(" \t");
        std::size_t ke = key.find_last_not_of(" \t");
        if (ks == std::string::npos) continue;
        key = key.substr(ks, ke - ks + 1);
        for (char& c : key) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        if (key != lname) continue;

        std::string val = line.substr(colon + 1);
        std::size_t vs = val.find_first_not_of(" \t");
        std::size_t ve = val.find_last_not_of(" \t");
        return vs == std::string::npos ? std::string() : val.substr(vs, ve - vs + 1);
    }
    return std::string();
}

// Lowercase a hex string in place-style (returns a copy). Incoming signatures
// may be upper- or lower-case hex; our expected digest is lowercase.
std::string to_lower(std::string s) {
    for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

//------------------------------------------------------------------------------

class Vitalera : public VendorBase {
public:
    explicit Vitalera(VendorConfig cfg) : VendorBase(make_info(), std::move(cfg)) {}

    // Fetch `domain` for `user_id` over [start_iso, end_iso]. Clinical data is
    // returned as a FHIR R5 Observation Bundle; every other domain hits the
    // native per-metric device-fetch endpoint. Empty bounds => Vitalera's
    // default window (params simply omitted). Returns the response JSON verbatim.
    std::string fetch(const std::string& user_id,
                      DataDomain domain,
                      const std::string& start_iso,
                      const std::string& end_iso) override {
        require_configured();
        const std::string uid(user_id);
        if (uid.empty()) {
            throw VendorError(info_.id + ": fetch requires a user_id (the monitored patient id)");
        }

        if (domain == DataDomain::Clinical) {
            return fetch_fhir_observations(uid, start_iso, end_iso);
        }
        return fetch_native_metric(uid, domain, start_iso, end_iso);
    }

    // Disconnect the user: deactivate the active JWT via the auth deactivate
    // endpoint. Best-effort — a non-2xx is surfaced as a VendorError. The path
    // is INFERRED within the confirmed auth/tokens family (the docs name a
    // deactivate op but not its exact route); reconcile against the gated
    // reference. Note this revokes the app's own token, not a per-patient link.
    void revoke(const std::string& /*user_id*/, const std::string& /*provider*/) override {
        require_configured();
        client::HttpRequest req;
        req.url     = base_url() + std::string(kAuthPath) + "deactivate/";
        req.body    = "{}";
        req.headers = auth_headers();
        client::HttpResponse res = client::HttpClient().post(req);
        if (res.status < 200 || res.status >= 300) {
            throw VendorError(http_error("revoke (auth/tokens/deactivate)", res));
        }
    }

    // List the provider/service catalogue Vitalera exposes to the caller's org:
    // GET /api/connected-accounts/services/ (entitlement-filtered). The method and
    // path are public; the response shape lives in the gated reference, so we
    // return the JSON verbatim.
    std::string list_providers(const std::string& /*user_id*/) override {
        require_configured();
        return get_json(base_url() + "connected-accounts/services/",
                        "list_providers (connected-accounts/services)");
    }

    // Verify and return an inbound Vitalera webhook. The signature is carried in
    // the `x-webhook-signature` header as lowercase-hex HMAC-SHA256 over the RAW
    // request body, keyed by the webhook endpoint secret (config.client_secret —
    // see header). The body's own `timestamp` is NOT part of the signed string.
    // On success returns the body verbatim; any mismatch / malformed input throws.
    std::string handle_webhook(const std::string& raw_headers,
                               const std::string& body) override {
        const std::string secret = config().client_secret;
        if (secret.empty()) {
            throw VendorError(info_.id + ": handle_webhook requires the webhook endpoint "
                              "secret — set MIROBODY_VENDOR_VITALERA_CLIENT_SECRET to your "
                              "webhook secret (use _API_KEY for the fetch bearer so the "
                              "client secret is free to hold the webhook secret)");
        }
        const std::string sig = header_value(raw_headers, "x-webhook-signature");
        if (sig.empty()) {
            throw VendorError(info_.id + ": webhook missing x-webhook-signature header");
        }

        std::array<unsigned char, 32> mac = storage::hmac_sha256(secret, std::string(body));
        const std::string expected = storage::hex_encode(mac.data(), mac.size());
        if (!ct_equal(expected, to_lower(sig))) {
            throw VendorError(info_.id + ": webhook signature verification failed (x-webhook-signature)");
        }

        rapidjson::Document doc;
        if (doc.Parse(std::string(body).c_str()).HasParseError() || !doc.IsObject()) {
            throw VendorError(info_.id + ": webhook body was not a JSON object");
        }
        return std::string(body);
    }

private:
    //--------------------------------------------------------------------------
    // Operations
    //--------------------------------------------------------------------------

    std::string fetch_native_metric(const std::string& uid,
                                     DataDomain domain,
                                     const std::string& start_iso,
                                     const std::string& end_iso) {
        const char* endpoint = native_endpoint(domain);
        if (!endpoint) {
            throw VendorError(info_.id + ": no native device-fetch endpoint for domain '" +
                              to_string(domain) + "'");
        }

        std::string url = base_url() + endpoint +
                          "?" + kPatientParam + "=" + url_encode(uid);
        if (!start_iso.empty()) {
            url += std::string("&") + kStartParam + "=" + url_encode(std::string(start_iso));
        }
        if (!end_iso.empty()) {
            url += std::string("&") + kEndParam + "=" + url_encode(std::string(end_iso));
        }
        return get_json(url, std::string("fetch ") + endpoint);
    }

    // FHIR R5 Observation search. `patient` and the `date` range (ge/le prefixes)
    // are standard FHIR search parameters, so only the base path is inferred.
    std::string fetch_fhir_observations(const std::string& uid,
                                        const std::string& start_iso,
                                        const std::string& end_iso) {
        std::string url = base_url() + kFhirSegment + "Observation" +
                          "?patient=" + url_encode(uid);
        if (!start_iso.empty()) {
            url += "&date=ge" + url_encode(std::string(start_iso));
        }
        if (!end_iso.empty()) {
            url += "&date=le" + url_encode(std::string(end_iso));
        }
        return get_json(url, "fetch fhir/Observation");
    }

    //--------------------------------------------------------------------------
    // Domain → endpoint mapping (confirmed native list endpoints)
    //--------------------------------------------------------------------------

    static const char* native_endpoint(DataDomain d) {
        switch (d) {
            case DataDomain::HeartRate:   return "heart-rate/";
            case DataDomain::Glucose:     return "blood-glucose/";
            case DataDomain::Activity:    return "step-count/";
            // BodyMetrics spans several Vitalera vitals; blood pressure is the
            // representative RPM metric. Callers needing SpO2/temperature can
            // reach those endpoints once the domain split is finer-grained.
            case DataDomain::BodyMetrics: return "blood-pressure/";
            // Clinical is served via FHIR, handled before this map is consulted.
            default:                      return nullptr;
        }
    }

    //--------------------------------------------------------------------------
    // Transport + auth
    //--------------------------------------------------------------------------

    std::string get_json(const std::string& url, const std::string& op) {
        client::HttpResponse res = client::HttpClient().get(url, /*timeout_ms=*/30000, auth_headers());
        if (res.status < 200 || res.status >= 300) {
            throw VendorError(http_error(op, res));
        }
        return res.body;
    }

    std::vector<std::string> auth_headers() {
        return { "Authorization: Bearer " + bearer() };
    }

    // Resolve a JWT bearer token, caching it for this client's lifetime.
    //   * client_id + client_secret  -> mint one at POST /authentication/
    //   * api_key only               -> treat it as a pre-issued bearer/JWT
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

    std::string mint_token() {
        client::HttpRequest req;
        req.url  = base_url() + kAuthPath;
        req.body = std::string("{\"grant_type\":\"") + kAuthGrantType + "\",\"" +
                   kAuthClientIdField + "\":\"" +
                   json_escape(config().client_id) + "\",\"" +
                   kAuthClientSecretField + "\":\"" +
                   json_escape(config().client_secret) + "\"}";
        client::HttpResponse res = client::HttpClient().post(req);
        if (res.status < 200 || res.status >= 300) {
            throw VendorError(http_error("authenticate (auth/tokens/)", res));
        }

        rapidjson::Document doc;
        if (doc.Parse(res.body.c_str()).HasParseError() || !doc.IsObject()) {
            throw VendorError(info_.id + ": authentication response was not a JSON object");
        }
        // OAuth 2.0 token response: the bearer is in "access_token". The legacy
        // SimpleJWT spellings are kept as fallbacks for forward/backward tolerance.
        for (const char* field : { "access_token", "access", "token", "jwt" }) {
            if (doc.HasMember(field) && doc[field].IsString()) {
                return doc[field].GetString();
            }
        }
        throw VendorError(info_.id + ": no access_token field in authentication response");
    }

    //--------------------------------------------------------------------------
    // Misc
    //--------------------------------------------------------------------------

    void require_configured() const {
        if (!config().configured()) {
            throw VendorError(info_.id + ": not configured — set " +
                              "MIROBODY_VENDOR_VITALERA_API_KEY or _CLIENT_ID/_CLIENT_SECRET");
        }
    }

    std::string base_url() const {
        std::string b = config().base_url.empty() ? std::string(kDefaultBase) : config().base_url;
        if (!b.empty() && b.back() != '/') b.push_back('/');
        return b;
    }

    std::string http_error(const std::string& op, const client::HttpResponse& res) const {
        // status <= 0 is a transport failure; curl puts the reason in body.
        return info_.id + ": " + op + " failed (HTTP " + std::to_string(res.status) +
               "): " + res.body.substr(0, 300);
    }

    static VendorInfo make_info() {
        VendorInfo i;
        i.id                   = "vitalera";
        i.display_name         = "Vitalera";
        i.positioning          = "Next-generation health data API platform";
        i.target_customers     = "Healthcare providers, RPM developers, digital health";
        i.data_source_coverage = "500+ wearable and medical devices (incl. ECG, blood glucose)";
        i.integration_method   = "REST API, automatic code generator, FHIR connectivity";
        i.compliance_summary   = "HIPAA, GDPR, ISO 27001, SaMD";
        i.differentiator       = "Auto-generates integration code, supports FHIR, certified as medical software";
        i.docs_url             = "https://www.vitalera.io";
        i.region               = Region::Global;
        i.open_source          = false;
        i.compliance           = {"HIPAA", "GDPR", "ISO_27001", "SaMD"};
        i.domains              = {DataDomain::HeartRate, DataDomain::Glucose, DataDomain::Clinical};
        i.integrations         = {Integration::Rest, Integration::Fhir};
        return i;
    }

    std::mutex  token_mu_;
    std::string token_;
};

}

std::unique_ptr<Vendor> make_vitalera(const VendorConfig& cfg) {
    return std::unique_ptr<Vendor>(new Vitalera(cfg));
}

}}

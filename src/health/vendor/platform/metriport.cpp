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
// authorize_url, handle_webhook, and revoke are implemented against the
// open-source Devices API (server routes + packages/api-sdk):
//   * authorize_url  -> POST /user?appUserId=<user_id> then GET /user/connect/token,
//     returning the Connect Widget URL https://connect.metriport.com/?token=…
//     (user_id carries your appUserId; redirect_uri sets the success/failure
//     redirects; sandbox flag added when base_url is the sandbox host).
//   * handle_webhook -> verifies the `x-metriport-signature` header (lowercase-hex
//     HMAC-SHA256 over the raw body, keyed by your webhook key in
//     config.client_secret — api_key is taken by x-api-key). ping_pong() builds
//     the {"pong":…} answer to Metriport's ping handshake.
//   * revoke         -> DELETE /user/{userId} (deletes the connected user, revoking
//     all their providers; revoke_provider() drops a single provider via
//     DELETE /user/{userId}/revoke?provider=).
// list_providers stays an inherited stub: the open source exposes NO supported-
// provider CATALOGUE endpoint — only the user-scoped GET /user/{userId}/
// connected-providers, which doesn't fit list_providers(). The supported set is
// the ProviderSource enum (apple, cronometer, dexcom, fitbit, garmin, google,
// oura, tenovi, whoop, withings).

#include "health/vendor/vendor.hpp"

#include "client/http_client.hpp"
#include "storage/sign.hpp"   // hmac_sha256 / hex_encode

#include <rapidjson/document.h>

#include <array>
#include <cctype>
#include <cstddef>
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

// Hosted Connect Widget the user is redirected to (connect-widget constants).
const char kWidgetBase[] = "https://connect.metriport.com";

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

// JSON-string-escape a value for the small ping/pong body we build by hand.
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

// Lowercase a string (hex signatures may arrive upper- or lower-case; our
// recomputed digest is lowercase hex).
std::string to_lower(std::string s) {
    for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
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

    // Begin the Devices-API connect flow and return the Connect Widget URL to
    // redirect the user to. Metriport provisions its own user from your app's user
    // id, so `user_id` carries that app id (sent as appUserId). state/provider have
    // no slot here (the widget lets the user pick a provider; the landing is set via
    // redirect_uri). Two server calls run:
    //   POST /user?appUserId=<user_id>      -> { userId }
    //   GET  /user/connect/token?userId=... -> { token }
    // and we return https://connect.metriport.com/?token=<token>, with the
    // success/failure redirects set to redirect_uri and the sandbox flag added when
    // base_url targets the sandbox host. Each call PROVISIONS a Metriport user from
    // appUserId (the SDK does the same); store and reuse the returned connection.
    std::string authorize_url(const std::string& redirect_uri,
                              const std::string& /*state*/,
                              const std::string& user_id,
                              const std::string& /*provider*/) override {
        require_configured();
        const std::string app_user_id(user_id);
        if (app_user_id.empty()) {
            throw VendorError(info_.id + ": authorize_url requires a user_id (your app's user "
                              "id, sent to Metriport as appUserId)");
        }

        const std::string metriport_user_id = create_user(app_user_id);
        const std::string token              = connect_token(metriport_user_id);

        std::string url = std::string(kWidgetBase) + "/?token=" + url_encode(token);
        if (is_sandbox()) {
            url += "&sandbox=true";
        }
        if (!redirect_uri.empty()) {
            const std::string r = url_encode(std::string(redirect_uri));
            url += "&redirectUrl=" + r + "&failRedirectUrl=" + r;
        }
        return url;
    }

    // Disconnect the user. With a `provider` slug, drop just that connection via
    // revoke_provider(); otherwise DELETE /user/{userId} deletes the Metriport
    // connected user, revoking all their provider connections. `user_id` is the
    // Metriport userId. A non-2xx is surfaced as a VendorError.
    void revoke(const std::string& user_id, const std::string& provider) override {
        require_configured();
        const std::string uid(user_id);
        if (uid.empty()) {
            throw VendorError(info_.id + ": revoke requires a user_id (the Metriport userId)");
        }
        if (!provider.empty()) {
            revoke_provider(uid, provider);
            return;
        }
        client::HttpRequest req;
        req.url     = base_url() + "/user/" + url_encode(uid);
        req.headers = { std::string(kApiKeyHeader) + ": " + config().api_key };
        client::HttpResponse res = client::HttpClient().request("DELETE", req);
        if (res.status < 200 || res.status >= 300) {
            throw VendorError(http_error("revoke (DELETE /user)", res));
        }
    }

    // Verify and return an inbound Metriport webhook. The signature is in the
    // `x-metriport-signature` header as lowercase-hex HMAC-SHA256 over the RAW
    // request body, keyed by your webhook key (config.client_secret — Metriport
    // uses api_key for x-api-key, so the webhook key rides on client_secret). The
    // same scheme covers both Devices and Medical webhooks. On success returns the
    // body verbatim; mismatch / malformed input throws.
    //
    // Ping handshake: Metriport verifies an endpoint by POSTing (through this same
    // signed pipeline) a body with a top-level "ping" field; the endpoint must
    // reply HTTP 200 with {"pong": <value>}. This method verifies + returns the
    // ping body like any other event; ping_pong() builds the answer the caller's
    // HTTP layer writes back.
    std::string handle_webhook(const std::string& raw_headers,
                               const std::string& body) override {
        const std::string secret = config().client_secret;
        if (secret.empty()) {
            throw VendorError(info_.id + ": handle_webhook requires your webhook key — set "
                              "MIROBODY_VENDOR_METRIPORT_CLIENT_SECRET to the key Metriport "
                              "generated when you set your webhook URL");
        }
        const std::string sig = header_value(raw_headers, "x-metriport-signature");
        if (sig.empty()) {
            throw VendorError(info_.id + ": webhook missing x-metriport-signature header");
        }

        std::array<unsigned char, 32> mac = storage::hmac_sha256(secret, std::string(body));
        const std::string expected = storage::hex_encode(mac.data(), mac.size());
        if (!ct_equal(expected, to_lower(sig))) {
            throw VendorError(info_.id + ": webhook signature verification failed (x-metriport-signature)");
        }

        rapidjson::Document doc;
        if (doc.Parse(std::string(body).c_str()).HasParseError() || !doc.IsObject()) {
            throw VendorError(info_.id + ": webhook body was not a JSON object");
        }
        return std::string(body);
    }

    //--------------------------------------------------------------------------
    // Connect-flow / disconnect helpers (not part of the Vendor interface)
    //--------------------------------------------------------------------------

    // Build the {"pong": <value>} body that answers a Metriport ping. Returns ""
    // when `webhook_json` is not a ping (no top-level "ping" string), so the
    // caller can branch: non-empty => write it back with HTTP 200.
    static std::string ping_pong(const std::string& webhook_json) {
        rapidjson::Document doc;
        if (doc.Parse(webhook_json.c_str()).HasParseError() || !doc.IsObject()) {
            return std::string();
        }
        if (!doc.HasMember("ping") || !doc["ping"].IsString()) {
            return std::string();
        }
        return std::string("{\"pong\":\"") + json_escape(doc["ping"].GetString()) + "\"}";
    }

    // Revoke a SINGLE provider for a user: DELETE /user/{userId}/revoke?provider=.
    // `provider` is a Metriport ProviderSource slug (fitbit, garmin, oura, …).
    void revoke_provider(const std::string& user_id, const std::string& provider) {
        require_configured();
        client::HttpRequest req;
        req.url     = base_url() + "/user/" + url_encode(user_id) +
                      "/revoke?provider=" + url_encode(provider);
        req.headers = { std::string(kApiKeyHeader) + ": " + config().api_key };
        client::HttpResponse res = client::HttpClient().request("DELETE", req);
        if (res.status < 200 || res.status >= 300) {
            throw VendorError(http_error("revoke_provider (DELETE /user/revoke)", res));
        }
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
    // Connect-flow transport
    //   POST /user?appUserId=<id>           -> { userId }
    //   GET  /user/connect/token?userId=... -> { token }
    //--------------------------------------------------------------------------
    std::string create_user(const std::string& app_user_id) {
        client::HttpRequest req;
        req.url     = base_url() + "/user?appUserId=" + url_encode(app_user_id);
        req.headers = { std::string(kApiKeyHeader) + ": " + config().api_key,
                        "Accept: application/json" };
        client::HttpResponse res = client::HttpClient().post(req);
        if (res.status < 200 || res.status >= 300) {
            throw VendorError(http_error("authorize_url (POST /user)", res));
        }
        return json_field(res.body, "userId", "POST /user");
    }

    std::string connect_token(const std::string& user_id) {
        const std::string url = base_url() + "/user/connect/token?userId=" + url_encode(user_id);
        return json_field(get_json(url, "authorize_url (GET /user/connect/token)"),
                          "token", "GET /user/connect/token");
    }

    // Pull a required top-level string field out of a JSON object response.
    std::string json_field(const std::string& body, const char* field, const char* op) const {
        rapidjson::Document doc;
        if (doc.Parse(body.c_str()).HasParseError() || !doc.IsObject()) {
            throw VendorError(info_.id + ": " + op + " response was not a JSON object");
        }
        if (!doc.HasMember(field) || !doc[field].IsString()) {
            throw VendorError(std::string(info_.id) + ": no '" + field + "' in " + op + " response");
        }
        return doc[field].GetString();
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
            throw VendorError(http_error(op, res));
        }
        return res.body;
    }

    std::string http_error(const std::string& op, const client::HttpResponse& res) const {
        // status <= 0 is a transport failure; curl puts the reason in body.
        return info_.id + ": " + op + " failed (HTTP " + std::to_string(res.status) +
               "): " + res.body.substr(0, 300);
    }

    // True when the configured host is Metriport's sandbox (so the Connect Widget
    // is told to run in sandbox mode).
    bool is_sandbox() const {
        return base_url().find("sandbox") != std::string::npos;
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

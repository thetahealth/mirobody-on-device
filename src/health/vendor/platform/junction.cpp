// Junction — diagnostic data integration platform. See README.md (Segment B).
//
// Junction (formerly "Vital" / tryvital) brokers data from 300+ wearables AND a
// nationwide U.S. lab-testing network through a single API, multi-language SDK,
// and a white-label embed ("Link"). Implemented against the public reference at
// https://docs.junction.com (the legacy https://docs.tryvital.io host 301s here).
//
// This client covers the operations the platform leads with: fetching wearable
// summaries (activity/sleep/heart-rate/body metrics) and lab orders/results, and
// listing the providers a user can connect.
//
// CONFIRMED from the public docs:
//   * base URLs        prod US   https://api.us.junction.com/
//                      prod EU   https://api.eu.junction.com/
//                      sandbox   https://api.sandbox.us.junction.com/  (and .eu)
//                      Legacy *.tryvital.io hosts remain supported.
//   * auth             Team API Key in the `X-Vital-API-Key` header (server to
//                      server). Key prefixes pk_us_*/pk_eu_* (prod), sk_us_*/sk_eu_*
//                      (sandbox) — the prefix, not the host, picks the environment,
//                      so override base_url to match the key you provision.
//   * wearable fetch   GET /v2/summary/<resource>/<user_id>?start_date=&end_date=
//                      (resources: activity, sleep, body, sleep_stream, …); dates
//                      accept YYYY-MM-DD or ISO datetimes.
//   * providers        GET /v2/providers
//   * lab orders       GET /v3/orders?user_id=&start_date=&end_date= (Labs domain)
//                      GET /v3/order/<order_id>/result (parsed JSON + PDF metadata)
//   * lab catalogue    GET /v3/lab_tests/labs
//   * link / connect   POST /v2/user            (body {"client_user_id": ...})
//                      POST /v2/link/token      (body {"user_id": ..., "provider"?})
//                                               -> { link_token, link_web_url }
//                      The hosted widget is launched at the returned link_web_url.
//   * webhooks         delivered via Svix. Each request carries svix-id,
//                      svix-timestamp and svix-signature headers; the signature is
//                      base64(HMAC-SHA256("<svix-id>.<svix-timestamp>.<raw_body>"))
//                      keyed by the base64-decoded portion of the endpoint signing
//                      secret (the part after the "whsec_" prefix). svix-signature
//                      is a space-separated list of "v1,<sig>" entries; a match on
//                      any entry (constant-time) verifies. (docs.junction.com/
//                      webhooks/introduction + docs.svix.com verification scheme.)
//
// authorize_url() mints a Link Token for an existing Junction user_id (carried by
// the interface's user_id arg) and returns the hosted-widget link_web_url; an
// optional provider pre-selects a data source. Create the user first via
// create_user() (its own step). redirect_uri / state have no slot in the Link
// Token request (the landing is configured Junction-side), so they are ignored.

#include "health/vendor/vendor.hpp"

#include "client/http_client.hpp"
#include "storage/sign.hpp"   // hmac_sha256 / base64_encode / base64_decode

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
// Confirmed defaults (see header comment).
//------------------------------------------------------------------------------

// Default to the U.S. *production* host. Override base_url for EU
// (https://api.eu.junction.com/) or sandbox
// (https://api.sandbox.us.junction.com/ / .eu) — note the API-key prefix must
// match the chosen environment.
const char kDefaultBase[] = "https://api.us.junction.com/";

// Confirmed auth header for server-to-server traffic.
const char kApiKeyHeader[] = "X-Vital-API-Key: ";

//------------------------------------------------------------------------------
// Local helpers
//------------------------------------------------------------------------------

// Percent-encode a query-string / path component (RFC 3986 unreserved pass
// through). Local copy, matching user/service.cpp and vitalera.cpp.
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

// JSON-string-escape a value for the small request bodies we build by hand.
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

//------------------------------------------------------------------------------

class Junction : public VendorBase {
public:
    explicit Junction(VendorConfig cfg) : VendorBase(make_info(), std::move(cfg)) {}

    // Begin consent for an existing Junction user: mint a Link Token and return the
    // hosted-widget URL to redirect the user to. `user_id` is the Junction user id
    // (create one first via create_user()); `provider` optionally pre-selects a
    // data source (empty lets the user choose in the widget). redirect_uri/state
    // have no slot in the Link Token request — the post-connect landing is set on
    // the Junction app side — so they are ignored here. Returns the link_web_url
    // from POST /v2/link/token.
    std::string authorize_url(const std::string& /*redirect_uri*/,
                              const std::string& /*state*/,
                              const std::string& user_id,
                              const std::string& provider) override {
        require_configured();
        const std::string uid(user_id);
        if (uid.empty()) {
            throw VendorError(info_.id + ": authorize_url requires a user_id (the Junction "
                              "user id; create one first via create_user())");
        }
        const std::string body = create_link_token(uid, provider);
        rapidjson::Document doc;
        if (doc.Parse(body.c_str()).HasParseError() || !doc.IsObject() ||
            !doc.HasMember("link_web_url") || !doc["link_web_url"].IsString()) {
            throw VendorError(info_.id + ": no link_web_url in link/token response");
        }
        return doc["link_web_url"].GetString();
    }

    // Fetch `domain` for `user_id` over [start_iso, end_iso]. Wearable domains
    // hit GET /v2/summary/<resource>/<user_id>; the Labs domain lists the user's
    // lab orders via GET /v3/orders?user_id=. `user_id` is the Junction user id
    // (the UUID returned by create-user). Empty bounds => the params are omitted
    // and Junction applies its default window. Returns the response JSON verbatim.
    std::string fetch(const std::string& user_id,
                      DataDomain domain,
                      const std::string& start_iso,
                      const std::string& end_iso) override {
        require_configured();
        const std::string uid(user_id);
        if (uid.empty()) {
            throw VendorError(info_.id + ": fetch requires a user_id (the Junction user id / UUID)");
        }

        if (domain == DataDomain::Labs) {
            return fetch_lab_orders(uid, start_iso, end_iso);
        }
        return fetch_summary(uid, domain, start_iso, end_iso);
    }

    // List the providers (device brands / labs) a user can connect, as Junction's
    // JSON: GET /v2/providers.
    std::string list_providers(const std::string& /*user_id*/) override {
        require_configured();
        return get_json(base_url() + "v2/providers", "list_providers (v2/providers)");
    }

    // Disconnect / deregister the user. Junction deletes a user (and their linked
    // connections) via DELETE /v2/user/<user_id>. `user_id` is the Junction user
    // id. A non-2xx is surfaced as a VendorError.
    void revoke(const std::string& user_id, const std::string& /*provider*/) override {
        require_configured();
        const std::string uid(user_id);
        if (uid.empty()) {
            throw VendorError(info_.id + ": revoke requires a user_id (the Junction user id / UUID)");
        }
        client::HttpRequest req;
        req.url     = base_url() + "v2/user/" + url_encode(uid);
        req.headers = api_headers();
        client::HttpResponse res = client::HttpClient().request("DELETE", req);
        if (res.status < 200 || res.status >= 300) {
            throw VendorError(http_error("revoke (DELETE v2/user)", res));
        }
    }

    // Verify and return an inbound Junction webhook (delivered via Svix). The
    // request carries svix-id / svix-timestamp / svix-signature headers; the
    // signature is base64(HMAC-SHA256("<id>.<timestamp>.<raw_body>")) keyed by the
    // base64-decoded portion of the endpoint signing secret — config.client_secret,
    // the "whsec_..." value from the Junction dashboard (the "whsec_" prefix is
    // stripped before decoding). svix-signature is a space-separated list of
    // "v1,<base64sig>" entries; a constant-time match on any entry verifies. On
    // success returns the body verbatim; any mismatch / malformed input throws.
    std::string handle_webhook(const std::string& raw_headers,
                               const std::string& body) override {
        std::string secret = config().client_secret;
        if (secret.empty()) {
            throw VendorError(info_.id + ": handle_webhook requires the endpoint signing "
                              "secret — set MIROBODY_VENDOR_JUNCTION_CLIENT_SECRET to the "
                              "\"whsec_...\" value from the Junction dashboard");
        }

        const std::string id  = header_value(raw_headers, "svix-id");
        const std::string ts  = header_value(raw_headers, "svix-timestamp");
        const std::string sig = header_value(raw_headers, "svix-signature");
        if (id.empty() || ts.empty() || sig.empty()) {
            throw VendorError(info_.id + ": webhook missing svix-id/svix-timestamp/"
                              "svix-signature header(s)");
        }

        // The signing key is the base64-decoded portion of the secret after the
        // documented "whsec_" prefix.
        const std::string prefix = "whsec_";
        if (secret.compare(0, prefix.size(), prefix) == 0) {
            secret = secret.substr(prefix.size());
        }
        const std::string key = storage::base64_decode(secret);

        const std::string signed_content = id + "." + ts + "." + std::string(body);
        std::array<unsigned char, 32> mac = storage::hmac_sha256(key, signed_content);
        const std::string expected = storage::base64_encode(mac.data(), mac.size());

        // svix-signature: space-separated "v<ver>,<base64sig>" entries; the body
        // verifies if ANY entry matches our recomputed digest (constant-time).
        bool ok = false;
        std::size_t start = 0;
        while (start <= sig.size()) {
            std::size_t sp = sig.find(' ', start);
            std::string entry = (sp == std::string::npos) ? sig.substr(start)
                                                          : sig.substr(start, sp - start);
            std::size_t comma = entry.find(',');
            if (comma != std::string::npos && ct_equal(expected, entry.substr(comma + 1))) {
                ok = true;
                break;
            }
            if (sp == std::string::npos) break;
            start = sp + 1;
        }
        if (!ok) {
            throw VendorError(info_.id + ": webhook signature verification failed (svix-signature)");
        }

        rapidjson::Document doc;
        if (doc.Parse(std::string(body).c_str()).HasParseError() || !doc.IsObject()) {
            throw VendorError(info_.id + ": webhook body was not a JSON object");
        }
        return std::string(body);
    }

    //--------------------------------------------------------------------------
    // Link-flow helpers (not part of the Vendor interface). create_user() is the
    // prerequisite step before authorize_url(); create_link_token() is what
    // authorize_url() calls under the hood, exposed here for callers that want the
    // full {link_token, link_web_url} JSON rather than just the URL.
    //--------------------------------------------------------------------------

    // Create a Junction user from your app's stable id. POST /v2/user with
    // {"client_user_id": ...}; returns the JSON (carrying the Junction user_id).
    std::string create_user(const std::string& client_user_id) {
        require_configured();
        client::HttpRequest req;
        req.url  = base_url() + "v2/user";
        req.body = std::string("{\"client_user_id\":\"") +
                   json_escape(client_user_id) + "\"}";
        req.headers = api_headers();
        client::HttpResponse res = client::HttpClient().post(req);
        if (res.status < 200 || res.status >= 300) {
            throw VendorError(http_error("create_user (POST v2/user)", res));
        }
        return res.body;
    }

    // Mint a short-lived Link Token for an existing Junction user. POST
    // /v2/link/token with {"user_id": ..., "provider"?}; the response carries
    // `link_token` and the hosted-widget `link_web_url`. `provider` may be empty
    // to let the user choose in the widget. Returns the JSON verbatim.
    std::string create_link_token(const std::string& junction_user_id,
                                  const std::string& provider) {
        require_configured();
        std::string body = std::string("{\"user_id\":\"") +
                           json_escape(junction_user_id) + "\"";
        if (!provider.empty()) {
            body += ",\"provider\":\"" + json_escape(provider) + "\"";
        }
        body += "}";

        client::HttpRequest req;
        req.url     = base_url() + "v2/link/token";
        req.body    = body;
        req.headers = api_headers();
        client::HttpResponse res = client::HttpClient().post(req);
        if (res.status < 200 || res.status >= 300) {
            throw VendorError(http_error("create_link_token (POST v2/link/token)", res));
        }
        return res.body;
    }

private:
    //--------------------------------------------------------------------------
    // Operations
    //--------------------------------------------------------------------------

    // Wearable summary: GET /v2/summary/<resource>/<user_id> with optional
    // start_date/end_date filters.
    std::string fetch_summary(const std::string& uid,
                              DataDomain domain,
                              const std::string& start_iso,
                              const std::string& end_iso) {
        const char* resource = summary_resource(domain);
        if (!resource) {
            throw VendorError(info_.id + ": no wearable summary endpoint for domain '" +
                              to_string(domain) + "' (try the Labs domain for diagnostics)");
        }

        std::string url = base_url() + "v2/summary/" + resource + "/" + url_encode(uid);
        bool have_query = false;
        if (!start_iso.empty()) {
            url += std::string(have_query ? "&" : "?") + "start_date=" +
                   url_encode(std::string(start_iso));
            have_query = true;
        }
        if (!end_iso.empty()) {
            url += std::string(have_query ? "&" : "?") + "end_date=" +
                   url_encode(std::string(end_iso));
            have_query = true;
        }
        return get_json(url, std::string("fetch v2/summary/") + resource);
    }

    // Lab diagnostics: list the user's orders. GET /v3/orders?user_id=, filtered
    // by creation date. The per-order parsed results live at
    // GET /v3/order/<order_id>/result (see fetch_order_result), keyed off the
    // order ids in this listing.
    std::string fetch_lab_orders(const std::string& uid,
                                 const std::string& start_iso,
                                 const std::string& end_iso) {
        std::string url = base_url() + "v3/orders?user_id=" + url_encode(uid);
        if (!start_iso.empty()) {
            url += "&start_date=" + url_encode(std::string(start_iso));
        }
        if (!end_iso.empty()) {
            url += "&end_date=" + url_encode(std::string(end_iso));
        }
        return get_json(url, "fetch v3/orders");
    }

    //--------------------------------------------------------------------------
    // Domain → wearable summary resource mapping (confirmed v2/summary resources)
    //--------------------------------------------------------------------------

    static const char* summary_resource(DataDomain d) {
        switch (d) {
            case DataDomain::Activity:    return "activity";
            case DataDomain::Sleep:       return "sleep";
            // Heart rate is carried within Junction's activity/sleep summaries
            // (and the workouts/sleep timeseries); "activity" is the closest
            // user+date summary surface.
            case DataDomain::HeartRate:   return "activity";
            // Junction groups weight/BMI/blood-pressure-style metrics under the
            // "body" summary resource.
            case DataDomain::BodyMetrics: return "body";
            // Labs is served via /v3/orders, handled before this map is consulted.
            default:                      return nullptr;
        }
    }

    //--------------------------------------------------------------------------
    // Transport + auth
    //--------------------------------------------------------------------------

    std::string get_json(const std::string& url, const std::string& op) {
        client::HttpResponse res = client::HttpClient().get(url, /*timeout_ms=*/30000, api_headers());
        if (res.status < 200 || res.status >= 300) {
            throw VendorError(http_error(op, res));
        }
        return res.body;
    }

    std::vector<std::string> api_headers() const {
        return { std::string(kApiKeyHeader) + config().api_key };
    }

    //--------------------------------------------------------------------------
    // Misc
    //--------------------------------------------------------------------------

    void require_configured() const {
        if (config().api_key.empty()) {
            throw VendorError(info_.id + ": not configured — set "
                              "MIROBODY_VENDOR_JUNCTION_API_KEY to a Junction Team API Key "
                              "(pk_us_*/pk_eu_* for prod, sk_us_*/sk_eu_* for sandbox)");
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
        i.id                   = "junction";
        i.display_name         = "Junction";
        i.positioning          = "Diagnostic data integration platform";
        i.target_customers     = "Virtual clinics, digital health, healthcare SaaS";
        i.data_source_coverage = "300+ wearables + nationwide U.S. lab testing network";
        i.integration_method   = "Single API, multi-language SDK, white-label embed";
        i.compliance_summary   = "U.S. healthcare compliance";
        i.differentiator       = "Integrated wearables and lab testing, no test markup, supports at-home blood draws";
        i.docs_url             = "https://www.junction.com";
        i.region               = Region::US;
        i.open_source          = false;
        i.compliance           = {"HIPAA"};
        i.domains              = {DataDomain::Activity, DataDomain::Labs};
        i.integrations         = {Integration::Rest, Integration::Sdk, Integration::WhiteLabel};
        return i;
    }
};

}

std::unique_ptr<Vendor> make_junction(const VendorConfig& cfg) {
    return std::unique_ptr<Vendor>(new Junction(cfg));
}

}}

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
//
// Why authorize_url() stays a stub: Junction's connect flow is user-scoped — a
// Link Token is minted for an already-created Junction user_id, and the consent
// page lives at the server-returned `link_web_url` (no client-supplied
// redirect_uri / state in the Vendor::authorize_url contract). The Vendor
// interface has no user_id parameter here, so rather than fabricate a redirect
// URL we surface a clear "not implemented" pointing at the link helpers; the
// real flow runs through create_user()/create_link_token() on the SDK side.

#include "health/vendor/vendor.hpp"

#include "client/http_client.hpp"

#include <rapidjson/document.h>

#include <cctype>
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

//------------------------------------------------------------------------------

class Junction : public VendorBase {
public:
    explicit Junction(VendorConfig cfg) : VendorBase(make_info(), std::move(cfg)) {}

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
    std::string list_providers() override {
        require_configured();
        return get_json(base_url() + "v2/providers", "list_providers (v2/providers)");
    }

    // Disconnect / deregister the user. Junction deletes a user (and their linked
    // connections) via DELETE /v2/user/<user_id>. `user_id` is the Junction user
    // id. A non-2xx is surfaced as a VendorError.
    void revoke(const std::string& user_id) override {
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

    //--------------------------------------------------------------------------
    // Link-flow helpers (not part of the Vendor interface; used by the SDK /
    // connect path, since authorize_url() lacks a user_id parameter — see header)
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

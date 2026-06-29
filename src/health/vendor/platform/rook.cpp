// Rook Health — unified wearable data API. See README.md (Segment A).
//
// Implemented against the public reference at https://docs.tryrook.io/api/ —
// Rook splits into a Connect API (data extraction) and a Score API (analytics),
// with REST + cross-platform SDK + webhook delivery. This client covers the
// Connect side: the per-pillar processed-data summary reads (the data the
// platform leads with), the consumer connection/authorize flow, and the HTTP
// Basic auth Rook gates every REST call behind.
//
// CONFIRMED from the public docs (https://docs.tryrook.io/api/):
//   * base URL        https://api.rook-connect.com   (sandbox host:
//                     https://api.rook-connect.review)
//   * auth            HTTP Basic auth — client_uuid as the username, the
//                     portal-issued secret key as the password, on every
//                     protected REST endpoint
//   * data fetch      GET /v2/processed_data/<pillar>/summary with `user_id`
//                     and `date` (YYYY-MM-DD) query params; pillars are
//                     physical_health, sleep_health, body_health
//   * connect flow    /api/v1/client_uuid/<uuid>/user_id/<user>/data_sources/
//                     authorizers?redirect_url=... presents the connections
//                     page a user authorizes data sources through
//   * webhook         deliveries carry an HMAC signature in the `X-ROOK-Hash`
//                     header (data-delivery guide)
//
// INFERRED — the field-level reference is partially gated, so these are
// best-effort and centralized below for easy reconciliation:
//   * the credential mapping onto Basic auth uses client_id=client_uuid and
//     client_secret=secret key; api_key, if set alone, is treated as a
//     pre-formed "uuid:secret" pair.
// The exact HMAC computation behind X-ROOK-Hash (algorithm, key, encoding) is
// documented only in a gated changelog/support guide, so handle_webhook is
// intentionally left as an inherited stub rather than fabricating a signature
// scheme we cannot confirm.
//
// list_providers and revoke are also left as stubs — not for lack of docs, but
// because the documented contracts do not fit the Vendor interface signatures
// (verified against https://docs.tryrook.io/api/):
//   * list_providers — every "list data sources" endpoint is USER-scoped (needs
//     user_id in the path: GET /api/v2/user_id/{user_id}/data_sources/authorized
//     lists only what a user already authorized; the full-catalogue authorizers
//     endpoint is DEPRECATED). There is no parameterless provider catalogue to
//     back a list_providers() call, so it stays a stub.
//   * revoke — POST /api/v1/user_id/{user_id}/data_sources/revoke_auth revokes a
//     SINGLE data source (body {"data_source": ...}); there is no whole-user
//     disconnect, so revoke(user_id) can't be honored without iterating sources
//     (which needs the authorized-sources list above). Left a stub.

#include "health/vendor/vendor.hpp"

#include "client/http_client.hpp"

#include <cctype>
#include <string>
#include <utility>

namespace mirobody { namespace vendor {
namespace {

//------------------------------------------------------------------------------
// Confirmed defaults + inferred names (see header comment).
//------------------------------------------------------------------------------

const char kDefaultBase[] = "https://api.rook-connect.com";

// Confirmed: processed-data summary query params.
const char kUserParam[] = "user_id";
const char kDateParam[] = "date";

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

// JSON-string-escape a value for the small revoke body we build by hand.
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

// Base64-encode (standard alphabet) — Rook's Basic auth header carries the
// "client_uuid:secret" pair base64-encoded per RFC 7617.
std::string base64_encode(const std::string& in) {
    static const char* tbl =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    out.reserve(((in.size() + 2) / 3) * 4);
    std::size_t i = 0;
    for (; i + 2 < in.size(); i += 3) {
        unsigned n = (static_cast<unsigned char>(in[i]) << 16) |
                     (static_cast<unsigned char>(in[i + 1]) << 8) |
                     (static_cast<unsigned char>(in[i + 2]));
        out.push_back(tbl[(n >> 18) & 0x3F]);
        out.push_back(tbl[(n >> 12) & 0x3F]);
        out.push_back(tbl[(n >> 6) & 0x3F]);
        out.push_back(tbl[n & 0x3F]);
    }
    if (i < in.size()) {
        unsigned n = static_cast<unsigned char>(in[i]) << 16;
        bool two = (i + 1 < in.size());
        if (two) n |= static_cast<unsigned char>(in[i + 1]) << 8;
        out.push_back(tbl[(n >> 18) & 0x3F]);
        out.push_back(tbl[(n >> 12) & 0x3F]);
        out.push_back(two ? tbl[(n >> 6) & 0x3F] : '=');
        out.push_back('=');
    }
    return out;
}

//------------------------------------------------------------------------------

class Rook : public VendorBase {
public:
    explicit Rook(VendorConfig cfg) : VendorBase(make_info(), std::move(cfg)) {}

    // Build the connections-page URL a user authorizes data sources through:
    //   /api/v1/client_uuid/<uuid>/user_id/<user>/data_sources/authorizers
    // `redirect_uri` maps onto Rook's `redirect_url` (where the user lands after
    // authorizing). Rook keys this URL on the end user, and the Vendor interface
    // has no user_id parameter on authorize_url, so we carry it in `state` (the
    // value the caller correlates the flow with) and substitute it for user_id.
    std::string authorize_url(const std::string& redirect_uri,
                              const std::string& state,
                              const std::string& user_id,
                              const std::string& /*provider*/) override {
        // The connections page is keyed on the Rook end-user id. Prefer the
        // explicit user_id; fall back to state for callers that still carry it
        // there. provider is unused — the authorizers page lists every source.
        const std::string user = user_id.empty() ? std::string(state) : user_id;
        if (user.empty()) {
            throw VendorError(info_.id + ": authorize_url requires a user_id (the Rook end-user id)");
        }
        const std::string uuid = client_uuid();
        std::string url = base_url() + "/api/v1/client_uuid/" + url_encode(uuid) +
                          "/user_id/" + url_encode(user) +
                          "/data_sources/authorizers";
        if (!redirect_uri.empty()) {
            url += "?redirect_url=" + url_encode(std::string(redirect_uri));
        }
        return url;
    }

    // List the data sources a user can connect, as Rook's JSON (the connections
    // authorizers endpoint: name/description/logo/auth-URL/status per source).
    // Rook exposes no parameterless catalogue, so `user_id` is required. This is
    // Rook's documented (but deprecated) full-list endpoint; the non-deprecated
    // GET /api/v2/user_id/{user_id}/data_sources/authorized lists only the sources
    // a user has already authorized.
    std::string list_providers(const std::string& user_id) override {
        require_configured();
        const std::string uid(user_id);
        if (uid.empty()) {
            throw VendorError(info_.id + ": list_providers requires a user_id — Rook has no "
                              "parameterless provider catalogue (the connectable-sources list "
                              "is user-scoped)");
        }
        const std::string url = base_url() + "/api/v1/client_uuid/" + url_encode(client_uuid()) +
                                "/user_id/" + url_encode(uid) + "/data_sources/authorizers";
        return get_json(url, "list_providers (data_sources/authorizers)");
    }

    // Disconnect a single data source for a user: POST /api/v1/user_id/{user_id}/
    // data_sources/revoke_auth with {"data_source": <slug>}. Rook revokes
    // per-source (there is no whole-user disconnect), so `provider` is required.
    void revoke(const std::string& user_id, const std::string& provider) override {
        require_configured();
        const std::string uid(user_id);
        if (uid.empty()) {
            throw VendorError(info_.id + ": revoke requires a user_id (the Rook end-user id)");
        }
        if (provider.empty()) {
            throw VendorError(info_.id + ": revoke requires a provider (the Rook data_source to "
                              "disconnect — Rook revokes per data source, not the whole user)");
        }
        client::HttpRequest req;
        req.url     = base_url() + "/api/v1/user_id/" + url_encode(uid) +
                      "/data_sources/revoke_auth";
        req.body    = std::string("{\"data_source\":\"") + json_escape(provider) + "\"}";
        req.headers = auth_headers();
        client::HttpResponse res = client::HttpClient().post(req);
        if (res.status < 200 || res.status >= 300) {
            throw VendorError(http_error("revoke (data_sources/revoke_auth)", res));
        }
    }

    // Fetch `domain` for `user_id` on a single day, as Rook's processed-data
    // summary JSON. Rook summaries are keyed on a single `date` (YYYY-MM-DD), so
    // `start_iso` selects the day (its leading YYYY-MM-DD is used); `end_iso` is
    // accepted for interface compatibility but Rook has no range param here.
    // Returns the response JSON verbatim.
    std::string fetch(const std::string& user_id,
                      DataDomain domain,
                      const std::string& start_iso,
                      const std::string& /*end_iso*/) override {
        require_configured();
        const std::string uid(user_id);
        if (uid.empty()) {
            throw VendorError(info_.id + ": fetch requires a user_id (the Rook end-user id)");
        }

        const char* pillar = pillar_for(domain);
        if (!pillar) {
            throw VendorError(info_.id + ": no processed-data pillar for domain '" +
                              to_string(domain) + "' (supported: activity, sleep, heart_rate)");
        }

        std::string url = base_url() + "/v2/processed_data/" + pillar + "/summary" +
                          "?" + kUserParam + "=" + url_encode(uid);
        // Rook's `date` is a calendar day; take the leading YYYY-MM-DD of the
        // ISO bound when provided.
        const std::string day = iso_day(std::string(start_iso));
        if (!day.empty()) {
            url += std::string("&") + kDateParam + "=" + url_encode(day);
        }
        return get_json(url, std::string("fetch processed_data/") + pillar);
    }

private:
    //--------------------------------------------------------------------------
    // Domain → processed-data pillar mapping (confirmed pillars)
    //--------------------------------------------------------------------------

    // Activity → physical_health, Sleep → sleep_health. HeartRate is a vital
    // carried in the body_health summary (Rook groups heart-rate/vitals there).
    static const char* pillar_for(DataDomain d) {
        switch (d) {
            case DataDomain::Activity:  return "physical_health";
            case DataDomain::Sleep:     return "sleep_health";
            case DataDomain::HeartRate: return "body_health";
            default:                    return nullptr;
        }
    }

    // Take the leading calendar day (YYYY-MM-DD) of an ISO-8601 string.
    static std::string iso_day(const std::string& iso) {
        if (iso.empty()) return std::string();
        if (iso.size() >= 10) return iso.substr(0, 10);
        return iso;
    }

    //--------------------------------------------------------------------------
    // Transport + auth
    //--------------------------------------------------------------------------

    std::string get_json(const std::string& url, const std::string& op) {
        client::HttpResponse res =
            client::HttpClient().get(url, /*timeout_ms=*/30000, auth_headers());
        if (res.status < 200 || res.status >= 300) {
            throw VendorError(http_error(op, res));
        }
        return res.body;
    }

    std::vector<std::string> auth_headers() const {
        return { "Authorization: Basic " + base64_encode(basic_pair()) };
    }

    // The "client_uuid:secret" pair Rook expects in Basic auth.
    //   * client_id + client_secret -> uuid=client_id, secret=client_secret
    //   * api_key only              -> treated as a pre-formed "uuid:secret"
    std::string basic_pair() const {
        if (!config().client_id.empty()) {
            return config().client_id + ":" + config().client_secret;
        }
        return config().api_key;  // expected to already be "uuid:secret"
    }

    std::string client_uuid() const {
        if (!config().client_id.empty()) return config().client_id;
        // api_key carries "uuid:secret"; the uuid is the part before ':'.
        const std::string& k = config().api_key;
        std::string::size_type colon = k.find(':');
        return colon == std::string::npos ? k : k.substr(0, colon);
    }

    //--------------------------------------------------------------------------
    // Misc
    //--------------------------------------------------------------------------

    void require_configured() const {
        if (!config().configured()) {
            throw VendorError(info_.id + ": not configured — set "
                              "MIROBODY_VENDOR_ROOK_CLIENT_ID/_CLIENT_SECRET to your "
                              "Rook client_uuid + secret key (or _API_KEY as \"uuid:secret\")");
        }
    }

    std::string base_url() const {
        std::string b = config().base_url.empty() ? std::string(kDefaultBase)
                                                   : config().base_url;
        // Endpoints below carry their own leading '/', so strip a trailing one.
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
        i.id                   = "rook";
        i.display_name         = "Rook Health";
        i.positioning          = "Unified wearable data API";
        i.target_customers     = "Digital health, fitness apps, insurtech";
        i.data_source_coverage = "Mainstream wearables (health/activity/sleep)";
        i.integration_method   = "REST API, cross-platform SDK, webhook";
        i.compliance_summary   = "Not publicly disclosed";
        i.differentiator       = "Modular architecture (Connect for data extraction + Score for analytics/scoring)";
        i.docs_url             = "https://docs.tryrook.io/docs/";
        i.region               = Region::Global;
        i.open_source          = false;
        i.domains              = {DataDomain::Activity, DataDomain::Sleep, DataDomain::HeartRate};
        i.integrations         = {Integration::Rest, Integration::Sdk, Integration::Webhook};
        return i;
    }
};

}

std::unique_ptr<Vendor> make_rook(const VendorConfig& cfg) {
    return std::unique_ptr<Vendor>(new Rook(cfg));
}

}}

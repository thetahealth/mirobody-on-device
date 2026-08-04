// Dexcom — direct client for the Dexcom CGM (continuous glucose monitor) brand.
// Dexcom sensors (G6, G7, One+, Stelo) speak a proprietary encrypted BLE protocol
// to the official Dexcom app; that app uploads to Dexcom's cloud, and THIS client
// reads from that cloud over the public Dexcom v3 Web API. So the reachable data
// is what Dexcom's cloud serves — not a device/BLE path (a third party cannot read
// the sensor over Bluetooth). See src/health/README.md.
//
// Dexcom v3 Web API (https://developer.dexcom.com/):
//   * base_url defaults to the SANDBOX host https://sandbox-api.dexcom.com — the
//     free, immediate, fake-data environment every registered developer gets, so a
//     fresh deploy touches no real PHI. Point at production via DEXCOM_ENVIRONMENT
//     (us => https://api.dexcom.com, eu/ous => https://api.dexcom.eu) or an explicit
//     DEXCOM_BASE_URL. See src/health/vendor_link.cpp for the mapping.
//   * auth is an OAuth 2.0 access token (config.api_key) as a Bearer header on
//     fetch(); the token is exchanged out of band at /v2/oauth2/token.
//   * authorize_url() builds the standard authorization-code consent URL at
//     {base}/v2/oauth2/login from config.client_id (scope offline_access; a
//     confidential server client authenticates with its secret at the token step).
//   * fetch() reads Estimated Glucose Values: GET /v3/users/self/egvs over a
//     [startDate, endDate] window. The API is user-scoped by the token ("self"),
//     so the user_id argument is unused (data belongs to the token holder). Note
//     the API is RETROSPECTIVE: EGVs are delayed ~1h (US) / ~3h (outside US) by
//     regulatory design — there is no real-time push here.
//
// Left as inherited stubs, deliberately (see vendor.hpp):
//   * revoke          — Dexcom documents no token-revocation endpoint on the
//                       partner API; revoking is done in the user's Dexcom account.
//   * list_providers  — Dexcom is a single brand, not a provider aggregator, so
//                       there is no provider catalogue to list.
//   * handle_webhook  — the standard partner API delivers no signed webhook, so
//                       there is no signature scheme to verify faithfully.
//
// Access tiers (all start free): Sandbox is immediate; production Limited Access
// (<=5 real users) needs only app registration; serving more real users requires a
// Dexcom commercial partnership + Data Licensing Agreement. See config.example.yml.

#include "health/vendor/vendor.hpp"
#include "health/vendor/oauth2.hpp"

#include "client/http_client.hpp"

#include <cctype>
#include <string>
#include <utility>
#include <vector>

namespace mirobody { namespace vendor {
namespace {

// OAuth authorize path (relative to base_url — sandbox and both production hosts
// all expose it). scope offline_access is Dexcom's only scope.
const char kAuthorizePath[] = "/v2/oauth2/login";
const char kDefaultScope[]  = "offline_access";

// Percent-encode a query-string component (RFC 3986 unreserved pass through).
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

// Normalize an ISO-8601 timestamp to Dexcom's YYYY-MM-DDThh:mm:ss query format
// (no timezone/fractional seconds — the API treats bounds as the user's local
// time). Returns "" when the string does not begin with a parseable date.
std::string dexcom_datetime(const std::string& iso) {
    if (iso.size() < 10 ||
        !std::isdigit(static_cast<unsigned char>(iso[0])) || iso[4] != '-' || iso[7] != '-') {
        return std::string();
    }
    if (iso.size() == 10) return iso + "T00:00:00";           // date only
    if (iso.size() >= 19 && iso[10] == 'T') return iso.substr(0, 19);  // drop 'Z'/frac/offset
    return iso;                                               // date + partial time; pass through
}

//------------------------------------------------------------------------------

class Dexcom : public VendorBase {
public:
    explicit Dexcom(VendorConfig cfg) : VendorBase(make_info(), std::move(cfg)) {}

    // Fetch `domain` over [start_iso, end_iso] as the Dexcom v3 JSON. Dexcom brokers
    // only glucose (EGVs). The token scopes the request to its owner, so `user_id`
    // is unused (the endpoint path is always "self"); accepted for interface parity.
    std::string fetch(const std::string& /*user_id*/,
                      DataDomain domain,
                      const std::string& start_iso,
                      const std::string& end_iso) override {
        require_token();
        if (domain != DataDomain::Glucose) {
            throw VendorError(std::string("dexcom: unsupported domain '") + to_string(domain) +
                              "' (Dexcom brokers: glucose)");
        }
        const std::string s = dexcom_datetime(start_iso);
        const std::string e = dexcom_datetime(end_iso);
        if (s.empty() || e.empty()) {
            throw VendorError(info_.id + ": fetch requires ISO-8601 start/end bounds "
                              "(egvs needs an explicit [startDate, endDate] window)");
        }

        const std::string url = base_url() + "/v3/users/self/egvs?startDate=" +
                                url_encode(s) + "&endDate=" + url_encode(e);
        const std::vector<std::string> headers = {
            "Authorization: Bearer " + config().api_key,
            "Accept: application/json",
        };
        client::HttpResponse res = client::HttpClient().get(url, /*timeout_ms=*/30000, headers);
        if (res.status < 200 || res.status >= 300) {
            throw VendorError(info_.id + ": egvs fetch failed (HTTP " +
                              std::to_string(res.status) + "): " + res.body.substr(0, 300));
        }
        return res.body;
    }

    // Build the OAuth 2.0 authorization-code consent URL the user is redirected to
    // (pure construction; the token is exchanged out of band at /v2/oauth2/token).
    // `redirect_uri`/`state` are the standard OAuth params; user_id/provider are
    // unused (Dexcom is a single brand; the user is identified by the resulting
    // token). Requires the OAuth client_id.
    std::string authorize_url(const std::string& redirect_uri,
                              const std::string& state,
                              const std::string& /*user_id*/,
                              const std::string& /*provider*/) override {
        if (config().client_id.empty()) {
            throw VendorError(info_.id + ": authorize_url requires an OAuth client_id — set "
                              "DEXCOM_CLIENT_ID (config or env)");
        }
        std::string url = base_url() + kAuthorizePath +
                          "?client_id=" + url_encode(config().client_id) +
                          "&response_type=code&scope=" + url_encode(kDefaultScope);
        if (!redirect_uri.empty()) {
            url += "&redirect_uri=" + url_encode(redirect_uri);
        }
        if (!state.empty()) {
            url += "&state=" + url_encode(state);
        }
        return url;
    }

    // OAuth2 token endpoint at {base}/v2/oauth2/token; client credentials in the body.
    TokenSet exchange_code(const std::string& code, const std::string& redirect_uri) override {
        require_client();
        return oauth2_token_request(base_url() + "/v2/oauth2/token", {
            {"grant_type", "authorization_code"},
            {"client_id", config().client_id},
            {"client_secret", config().client_secret},
            {"code", code},
            {"redirect_uri", redirect_uri},
        });
    }
    TokenSet refresh(const std::string& refresh_token) override {
        require_client();
        return oauth2_token_request(base_url() + "/v2/oauth2/token", {
            {"grant_type", "refresh_token"},
            {"client_id", config().client_id},
            {"client_secret", config().client_secret},
            {"refresh_token", refresh_token},
        });
    }

private:
    void require_client() const {
        if (config().client_id.empty() || config().client_secret.empty()) {
            throw VendorError(info_.id + ": token exchange needs DEXCOM_CLIENT_ID + DEXCOM_CLIENT_SECRET");
        }
    }

    // The API host, trailing slash trimmed. Defaults to the sandbox (safe for an
    // unconfigured deploy — fake data, no real PHI); vendor_config() maps
    // DEXCOM_ENVIRONMENT to a production host, or set DEXCOM_BASE_URL.
    std::string base_url() const {
        std::string b = config().base_url;
        if (b.empty()) b = "https://sandbox-api.dexcom.com";
        while (!b.empty() && b.back() == '/') b.pop_back();
        return b;
    }

    void require_token() const {
        if (config().api_key.empty()) {
            throw VendorError(info_.id + ": api_key is required — set "
                              "DEXCOM_API_KEY (config or env) to a Dexcom OAuth 2.0 access token "
                              "(exchanged out of band at /v2/oauth2/token)");
        }
    }

    static VendorInfo make_info() {
        VendorInfo i;
        i.id                   = "dexcom";
        i.display_name         = "Dexcom";
        i.positioning          = "CGM brand with a public OAuth2 cloud Web API (retrospective glucose)";
        i.target_customers     = "Apps serving Dexcom CGM users (diabetes management, RPM)";
        i.data_source_coverage = "Dexcom CGM: G6, G7/One+, Stelo — estimated glucose values, events, devices";
        i.integration_method   = "REST v3 Web API (OAuth2); cloud read only — sensor BLE is proprietary/encrypted";
        i.compliance_summary   = "User-consented OAuth; CGM data is PHI under HIPAA (BAA required for production)";
        i.differentiator       = "Direct Dexcom cloud API — free sandbox + <=5-user production without a partnership";
        i.docs_url             = "https://developer.dexcom.com/";
        i.region               = Region::US;
        i.open_source          = false;
        i.compliance           = {"HIPAA"};
        i.domains              = {DataDomain::Glucose};
        i.integrations         = {Integration::Rest};
        return i;
    }
};

}

std::unique_ptr<Vendor> make_dexcom(const VendorConfig& cfg) {
    return std::unique_ptr<Vendor>(new Dexcom(cfg));
}

}}

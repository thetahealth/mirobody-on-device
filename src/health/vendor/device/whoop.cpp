// WHOOP — direct client for the WHOOP band, which has a free, self-serve public
// developer API. A direct integration for deployments (or individual developers who
// own a WHOOP — a device + membership is required to use the platform at all) that
// want WHOOP data without an aggregator. See src/health/README.md.
//
// WHOOP Developer Platform (https://developer.whoop.com/):
//   * base_url defaults to the documented host https://api.prod.whoop.com
//     (override via WHOOP_BASE_URL).
//   * auth is an OAuth 2.0 access token (config.api_key) as a Bearer header on
//     fetch(); the token is exchanged out of band at /oauth/oauth2/token.
//   * authorize_url() builds the authorization-code consent URL at
//     {base}/oauth/oauth2/auth from config.client_id.
//   * fetch() issues the documented collection GETs over a [start, end] ISO-8601
//     range (the API paginates via `nextToken`; this returns the first page — a
//     caller wanting more follows nextToken). WHOOP has no single "heart rate"
//     collection: HR/HRV live inside the Recovery record, so HeartRate maps there.
//
// The API version is centralized in kApiVersion. WHOOP's v1 was deprecated in favor
// of v2; the v2 collection paths (activity/sleep, recovery, cycle) are used here.
// Reconcile against developer.whoop.com when onboarding.
//
// revoke / list_providers / handle_webhook stay inherited stubs: WHOOP is a single
// brand, documents no self-serve token-revoke endpoint, and its webhook signing
// contract is a separate scheme not wired here.

#include "health/vendor/vendor.hpp"
#include "health/vendor/oauth2.hpp"

#include "client/http_client.hpp"

#include <cctype>
#include <string>
#include <utility>
#include <vector>

namespace mirobody { namespace vendor {
namespace {

// OAuth + API version constants (see header note on v1 -> v2).
const char kAuthorizePath[] = "/oauth/oauth2/auth";
const char kApiVersion[]    = "v2";
// Consent scopes matching the domains brokered; `offline` requests a refresh token.
const char kDefaultScope[]  = "read:recovery read:sleep read:cycles read:workout read:profile offline";

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

class Whoop : public VendorBase {
public:
    explicit Whoop(VendorConfig cfg) : VendorBase(make_info(), std::move(cfg)) {}

    // Fetch `domain` over [start_iso, end_iso] as the WHOOP JSON. The token scopes
    // the request to its owner, so `user_id` is unused (accepted for parity).
    std::string fetch(const std::string& /*user_id*/,
                      DataDomain domain,
                      const std::string& start_iso,
                      const std::string& end_iso) override {
        require_token();

        std::string url = base_url() + collection(domain);
        std::string query;
        if (!start_iso.empty()) query += (query.empty() ? "?" : "&") + std::string("start=") + url_encode(start_iso);
        if (!end_iso.empty())   query += (query.empty() ? "?" : "&") + std::string("end=")   + url_encode(end_iso);
        url += query;

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

    // Build the OAuth 2.0 authorization-code consent URL (pure construction; the
    // token is exchanged out of band). Requires the OAuth client_id.
    std::string authorize_url(const std::string& redirect_uri,
                              const std::string& state,
                              const std::string& /*user_id*/,
                              const std::string& /*provider*/) override {
        if (config().client_id.empty()) {
            throw VendorError(info_.id + ": authorize_url requires an OAuth client_id — set "
                              "WHOOP_CLIENT_ID (config or env)");
        }
        std::string url = base_url() + kAuthorizePath +
                          "?response_type=code&client_id=" + url_encode(config().client_id) +
                          "&scope=" + url_encode(kDefaultScope);
        if (!redirect_uri.empty()) url += "&redirect_uri=" + url_encode(redirect_uri);
        if (!state.empty())        url += "&state=" + url_encode(state);
        return url;
    }

    // OAuth2 token endpoint at {base}/oauth/oauth2/token; client credentials in the
    // body. `offline` scope on refresh keeps a refresh token issued.
    TokenSet exchange_code(const std::string& code, const std::string& redirect_uri) override {
        require_client();
        return oauth2_token_request(base_url() + "/oauth/oauth2/token", {
            {"grant_type", "authorization_code"},
            {"code", code},
            {"redirect_uri", redirect_uri},
            {"client_id", config().client_id},
            {"client_secret", config().client_secret},
        });
    }
    TokenSet refresh(const std::string& refresh_token) override {
        require_client();
        return oauth2_token_request(base_url() + "/oauth/oauth2/token", {
            {"grant_type", "refresh_token"},
            {"refresh_token", refresh_token},
            {"client_id", config().client_id},
            {"client_secret", config().client_secret},
            {"scope", "offline"},
        });
    }

private:
    void require_client() const {
        if (config().client_id.empty() || config().client_secret.empty()) {
            throw VendorError(info_.id + ": token exchange needs WHOOP_CLIENT_ID + WHOOP_CLIENT_SECRET");
        }
    }

    // Documented v2 collection paths. WHOOP has no standalone heart-rate collection —
    // HR/HRV are fields of the Recovery record, so HeartRate maps to /recovery.
    // Throws for domains WHOOP does not broker.
    static std::string collection(DataDomain d) {
        const std::string v = std::string("/developer/") + kApiVersion;
        switch (d) {
            case DataDomain::Sleep:     return v + "/activity/sleep";
            case DataDomain::HeartRate: return v + "/recovery";
            case DataDomain::Activity:  return v + "/cycle";
            default:
                throw VendorError(std::string("whoop: unsupported domain '") + to_string(d) +
                                  "' (WHOOP brokers: sleep, heart_rate [recovery], activity [cycle])");
        }
    }

    std::string base_url() const {
        std::string b = config().base_url;
        if (b.empty()) b = "https://api.prod.whoop.com";
        while (!b.empty() && b.back() == '/') b.pop_back();
        return b;
    }

    void require_token() const {
        if (config().api_key.empty()) {
            throw VendorError(info_.id + ": api_key is required — set "
                              "WHOOP_API_KEY (config or env) to a WHOOP OAuth 2.0 access token");
        }
    }

    static VendorInfo make_info() {
        VendorInfo i;
        i.id                   = "whoop";
        i.display_name         = "WHOOP";
        i.positioning          = "Recovery/strain band with a free, self-serve OAuth2 developer API";
        i.target_customers     = "Apps (and individual developers) serving WHOOP members";
        i.data_source_coverage = "WHOOP band: recovery (HRV, resting HR), sleep, strain (cycles), workouts";
        i.integration_method   = "REST v2 Web API (OAuth2), cursor-paginated";
        i.compliance_summary   = "User-consented OAuth scopes";
        i.differentiator       = "Free self-serve developer platform (device + membership required to use)";
        i.docs_url             = "https://developer.whoop.com/";
        i.region               = Region::Global;
        i.open_source          = false;
        i.domains              = {DataDomain::Sleep, DataDomain::HeartRate, DataDomain::Activity};
        i.integrations         = {Integration::Rest};
        return i;
    }
};

}

std::unique_ptr<Vendor> make_whoop(const VendorConfig& cfg) {
    return std::unique_ptr<Vendor>(new Whoop(cfg));
}

}}

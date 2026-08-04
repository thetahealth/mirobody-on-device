// Polar — direct client for the Polar heart-rate / watch brand, which exposes a
// free, self-serve public API (Polar Open AccessLink). A direct integration for
// deployments (or individual developers who own a Polar device) that want Polar
// data without an aggregator. See src/health/README.md.
//
// Polar Open AccessLink v3 (https://www.polar.com/accesslink-api/):
//   * base_url defaults to the documented host https://www.polaraccesslink.com
//     (override via POLAR_BASE_URL).
//   * auth is an OAuth 2.0 access token (config.api_key) as a Bearer header; the
//     token is exchanged out of band at https://polarremote.com/v2/oauth2/token.
//   * authorize_url() builds the authorization-code consent URL at
//     https://flow.polar.com/oauth2/authorization from config.client_id.
//   * revoke() DELETEs the registered user (/v3/users/{user-id}), disconnecting them.
//
// fetch() is only PARTLY expressible on this interface, because AccessLink splits
// data into two shapes:
//   * Sleep (and Nightly Recharge) are NON-transactional direct GETs by user —
//     /v3/users/{user-id}/sleep returns the recent nights — so Sleep is honored.
//   * Training sessions and daily activity use a TRANSACTION pull model (create a
//     transaction, list its resource URLs, GET each, then commit) that returns
//     "new data since the last pull" — there is no [start,end] query. That does not
//     fit fetch(user_id, domain, start, end), so Activity/HeartRate throw a
//     VendorError explaining the model rather than fabricating a date-range call.
//     (Same honest-stub stance as garmin.cpp's push model.)
//
// list_providers / handle_webhook stay inherited stubs: Polar is a single brand,
// and its webhook ("ping") contract is a separate scheme not wired here.

#include "health/vendor/vendor.hpp"
#include "health/vendor/oauth2.hpp"

#include "client/http_client.hpp"

#include <cctype>
#include <string>
#include <utility>
#include <vector>

namespace mirobody { namespace vendor {
namespace {

// Authorize lives on flow.polar.com; the API lives on base_url(); the token
// endpoint (out of band) is polarremote.com.
const char kAuthorizeUrl[] = "https://flow.polar.com/oauth2/authorization";
const char kDefaultScope[] = "accesslink.read_all";

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

class Polar : public VendorBase {
public:
    explicit Polar(VendorConfig cfg) : VendorBase(make_info(), std::move(cfg)) {}

    // Fetch `domain` for `user_id` (the Polar user-id from registration). Sleep is a
    // direct GET; training/activity use AccessLink's transaction model and cannot be
    // honored by this stateless [start,end] signature (see the header). Polar's
    // sleep list is not date-ranged (it returns recent nights), so the bounds are
    // accepted but not sent.
    std::string fetch(const std::string& user_id,
                      DataDomain domain,
                      const std::string& /*start_iso*/,
                      const std::string& /*end_iso*/) override {
        require_token();
        if (user_id.empty()) {
            throw VendorError(info_.id + ": fetch requires a user_id (the registered Polar user-id)");
        }
        if (domain != DataDomain::Sleep) {
            throw VendorError(std::string("polar: domain '") + to_string(domain) +
                              "' uses AccessLink's transaction pull model (create/list/get/commit), "
                              "which has no [start,end] query and does not fit fetch(); only 'sleep' "
                              "is a direct GET here. See polar.cpp.");
        }

        const std::string url = base_url() + "/v3/users/" + url_encode(user_id) + "/sleep";
        const std::vector<std::string> headers = {
            "Authorization: Bearer " + config().api_key,
            "Accept: application/json",
        };
        client::HttpResponse res = client::HttpClient().get(url, /*timeout_ms=*/30000, headers);
        if (res.status < 200 || res.status >= 300) {
            throw VendorError(info_.id + ": sleep fetch failed (HTTP " +
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
                              "POLAR_CLIENT_ID (config or env)");
        }
        std::string url = std::string(kAuthorizeUrl) +
                          "?response_type=code&client_id=" + url_encode(config().client_id) +
                          "&scope=" + url_encode(kDefaultScope);
        if (!redirect_uri.empty()) url += "&redirect_uri=" + url_encode(redirect_uri);
        if (!state.empty())        url += "&state=" + url_encode(state);
        return url;
    }

    // Token endpoint at polarremote.com; HTTP Basic (client_id:client_secret). Polar's
    // token step does not use redirect_uri. Polar access tokens DO NOT expire and no
    // refresh token is issued, so refresh() is unsupported.
    TokenSet exchange_code(const std::string& code, const std::string& /*redirect_uri*/) override {
        if (config().client_id.empty() || config().client_secret.empty()) {
            throw VendorError(info_.id + ": token exchange needs POLAR_CLIENT_ID + POLAR_CLIENT_SECRET");
        }
        return oauth2_token_request("https://polarremote.com/v2/oauth2/token",
            { {"grant_type", "authorization_code"}, {"code", code} },
            config().client_id, config().client_secret);
    }
    TokenSet refresh(const std::string& /*refresh_token*/) override {
        throw VendorError(info_.id + ": Polar access tokens do not expire — no refresh grant");
    }

    // Disconnect the user: DELETE /v3/users/{user-id} with the Bearer token.
    // `provider` is unused (Polar is a single brand).
    void revoke(const std::string& user_id, const std::string& /*provider*/) override {
        require_token();
        if (user_id.empty()) {
            throw VendorError(info_.id + ": revoke requires a user_id (the registered Polar user-id)");
        }
        client::HttpRequest req;
        req.url     = base_url() + "/v3/users/" + url_encode(user_id);
        req.headers = { "Authorization: Bearer " + config().api_key };
        client::HttpResponse res = client::HttpClient().request("DELETE", req);
        if (res.status < 200 || res.status >= 300) {
            throw VendorError(info_.id + ": revoke failed (HTTP " +
                              std::to_string(res.status) + "): " + res.body.substr(0, 300));
        }
    }

private:
    std::string base_url() const {
        std::string b = config().base_url;
        if (b.empty()) b = "https://www.polaraccesslink.com";
        while (!b.empty() && b.back() == '/') b.pop_back();
        return b;
    }

    void require_token() const {
        if (config().api_key.empty()) {
            throw VendorError(info_.id + ": api_key is required — set "
                              "POLAR_API_KEY (config or env) to a Polar OAuth 2.0 access token");
        }
    }

    static VendorInfo make_info() {
        VendorInfo i;
        i.id                   = "polar";
        i.display_name         = "Polar";
        i.positioning          = "Heart-rate / sports-watch brand with a free, self-serve OAuth2 API";
        i.target_customers     = "Apps (and individual developers) serving Polar device users";
        i.data_source_coverage = "Polar Flow: training sessions, daily activity, sleep, Nightly Recharge";
        i.integration_method   = "REST Open AccessLink v3 (OAuth2); training/activity are transaction-pull";
        i.compliance_summary   = "User-consented OAuth scopes";
        i.differentiator       = "Free self-serve API — a Polar Flow account registers a client, no approval";
        i.docs_url             = "https://www.polar.com/accesslink-api/";
        i.region               = Region::Global;
        i.open_source          = false;
        i.domains              = {DataDomain::Activity, DataDomain::HeartRate, DataDomain::Sleep};
        i.integrations         = {Integration::Rest};
        return i;
    }
};

}

std::unique_ptr<Vendor> make_polar(const VendorConfig& cfg) {
    return std::unique_ptr<Vendor>(new Polar(cfg));
}

}}

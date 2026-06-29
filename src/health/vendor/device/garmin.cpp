// Garmin Health API — premium device brand. Garmin migrated the Health/Connect
// APIs from OAuth 1.0a to OAuth 2.0 + PKCE, and the OAuth flow is public (the
// "OAuth2.0 PKCE Specification" PDF on developerportal.garmin.com). The DATA side
// is still partner-gated and push-based, which shapes what this client can do from
// public docs:
//   1. Partner-gated data: the Health REST API spec (endpoints/payloads) needs
//      Garmin's program approval; there is no self-serve key, and Garmin retains
//      only ~7 days of data for pull. So fetch() explains the model rather than
//      faking a synchronous pull.
//   2. OAuth 2.0 + PKCE (PKCE mandatory): authorize at
//      https://connect.garmin.com/oauth2Confirm, token at
//      https://diauth.garmin.com/di-oauth2-service/oauth/token. The code_verifier
//      minted at authorize-time must be persisted and replayed at the token
//      exchange, but Vendor::authorize_url returns only a URL with nowhere to stash
//      it — so PKCE consent belongs at a stateful layer (cf. ehr_connect.cpp, which
//      caches the verifier+state), and authorize_url stays an inherited stub here.
//   3. Push delivery: Garmin POSTs summaries to a registered webhook; backfill
//      endpoints return 202 and trigger async push rather than returning data. The
//      pushes carry NO documented signature, so handle_webhook cannot verify
//      authenticity from public docs and stays an inherited stub.
//
// revoke() IS wired: the public PKCE spec documents the required account-disconnect
// hook, DELETE /wellness-api/rest/user/registration (OAuth2 Bearer). list_providers
// is N/A (single brand). See https://developer.garmin.com/gc-developer-program/health-api/.

#include "health/vendor/vendor.hpp"

#include "client/http_client.hpp"

#include <string>
#include <utility>
#include <vector>

namespace mirobody { namespace vendor {
namespace {

class Garmin : public VendorBase {
public:
    explicit Garmin(VendorConfig cfg) : VendorBase(make_info(), std::move(cfg)) {}

    // Garmin Health is push-based: data is delivered to a registered webhook, not
    // pulled. (Ping/Pull endpoints like /wellness-api/rest/dailies exist but their
    // request/response spec is partner-gated and only ~7 days are retained.) Fail
    // with the reason rather than the generic VendorBase stub, so the caller learns
    // the model.
    std::string fetch(const std::string&, DataDomain,
                      const std::string&, const std::string&) override {
        throw VendorError(info_.id + ": Garmin Health is push-based and partner-gated "
                          "(OAuth2.0). Data is pushed to a registered webhook, not pulled — "
                          "wire the Ping/Pull data endpoints (partner-gated spec) or consume the "
                          "push once approved partner credentials exist.");
    }

    // Deregister the user — Garmin's required "disconnect / delete my account" hook:
    // DELETE /wellness-api/rest/user/registration with the user's OAuth 2.0 access
    // token as a Bearer header (config.api_key). Documented in the public OAuth2
    // PKCE spec. provider/user_id are unused (the token identifies the user).
    // Partner-gated: fails without approved credentials.
    void revoke(const std::string& /*user_id*/, const std::string& /*provider*/) override {
        if (config().api_key.empty()) {
            throw VendorError(info_.id + ": revoke requires a Garmin OAuth 2.0 access token — set "
                              "MIROBODY_VENDOR_GARMIN_API_KEY (partner-gated; needs approved creds)");
        }
        client::HttpRequest req;
        req.url     = base_url() + "/wellness-api/rest/user/registration";
        req.headers = { "Authorization: Bearer " + config().api_key };
        client::HttpResponse res = client::HttpClient().request("DELETE", req);
        if (res.status < 200 || res.status >= 300) {
            throw VendorError(info_.id + ": revoke failed (HTTP " +
                              std::to_string(res.status) + "): " + res.body.substr(0, 300));
        }
    }

    // list_providers / authorize_url / handle_webhook inherit the VendorBase stubs
    // (see header: PKCE-stateful consent, single brand, unsigned push webhook).

private:
    // Garmin's data-API host, no trailing slash (paths below add their own '/').
    std::string base_url() const {
        std::string b = config().base_url.empty() ? std::string("https://apis.garmin.com")
                                                   : config().base_url;
        while (!b.empty() && b.back() == '/') b.pop_back();
        return b;
    }

    static VendorInfo make_info() {
        VendorInfo i;
        i.id                   = "garmin";
        i.display_name         = "Garmin Health";
        i.positioning          = "Premium wearable brand; partner-gated, push-based Health API";
        i.target_customers     = "Approved partners serving Garmin device users";
        i.data_source_coverage = "Garmin watches, bike computers, and health sensors";
        i.integration_method   = "OAuth2.0 + PKCE; push (Ping/Push webhook) delivery; backfill triggers async";
        i.compliance_summary   = "Partner-program approval; user-consented OAuth2.0";
        i.differentiator       = "Deep multisport / endurance metrics from premium devices";
        i.docs_url             = "https://developer.garmin.com/gc-developer-program/health-api/";
        i.region               = Region::Global;
        i.open_source          = false;
        i.domains              = {DataDomain::Activity, DataDomain::HeartRate,
                                  DataDomain::Sleep, DataDomain::BodyMetrics};
        i.integrations         = {Integration::Rest, Integration::Webhook};
        return i;
    }
};

}

std::unique_ptr<Vendor> make_garmin(const VendorConfig& cfg) {
    return std::unique_ptr<Vendor>(new Garmin(cfg));
}

}}

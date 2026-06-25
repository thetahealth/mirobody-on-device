// Garmin Health API — a device brand whose API does NOT fit the synchronous pull
// model the other clients use, so this client is honest about that rather than
// faking a fetch. Three things make Garmin different:
//   1. Partner-gated: access requires Garmin's approval into the Health API
//      program; there is no self-serve key.
//   2. OAuth 1.0a: requests are signed with a consumer key/secret + per-user
//      access token/secret pair, not a simple Bearer token.
//   3. Push delivery: Garmin PUSHes summary data to a registered webhook (the
//      "Ping/Push" service); the backfill endpoints only TRIGGER asynchronous
//      delivery — they do not return the data in the response.
//
// So `fetch()` cannot synchronously return data and instead explains the model;
// the real integration lands in `handle_webhook()` (parse the pushed summaries)
// plus an out-of-band OAuth1.0a-signed backfill trigger, both of which need the
// deployer's approved partner credentials. Until those exist, the network ops
// stay documented stubs. See https://developer.garmin.com/gc-developer-program/health-api/.

#include "health/vendor/vendor.hpp"

#include <string>
#include <utility>

namespace mirobody { namespace vendor {
namespace {

class Garmin : public VendorBase {
public:
    explicit Garmin(VendorConfig cfg) : VendorBase(make_info(), std::move(cfg)) {}

    // Garmin has no synchronous pull. Fail with the reason rather than the generic
    // VendorBase "not implemented (stub)", so the caller learns the model.
    std::string fetch(const std::string&, DataDomain,
                      const std::string&, const std::string&) override {
        throw VendorError(info_.id + ": Garmin Health is push-based and partner-gated "
                          "(OAuth1.0a). Data is delivered to a registered webhook, not pulled — "
                          "wire handle_webhook() and an OAuth1.0a-signed backfill trigger once "
                          "approved partner credentials exist.");
    }

    // list_providers / authorize_url / handle_webhook / revoke inherit the
    // VendorBase stubs: the consent flow is OAuth1.0a and the webhook parser needs
    // the registered partner credentials, so none is fabricated here.

private:
    static VendorInfo make_info() {
        VendorInfo i;
        i.id                   = "garmin";
        i.display_name         = "Garmin Health";
        i.positioning          = "Premium wearable brand; partner-gated, push-based Health API";
        i.target_customers     = "Approved partners serving Garmin device users";
        i.data_source_coverage = "Garmin watches, bike computers, and health sensors";
        i.integration_method   = "OAuth1.0a + push (Ping/Push webhook) delivery; backfill triggers async";
        i.compliance_summary   = "Partner-program approval; user-consented OAuth1.0a";
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

// Validic — enterprise PGHD infrastructure. See README.md (Segment B).
//
// Implemented against the public Validic Inform REST API reference at
// https://developer.validic.com (formerly helpdocs.validic.com). Inform is the
// current platform; the legacy V1 API (docs.validic.com) is distinct and not
// targeted here. This client covers the two operations the REST API leads with:
// provisioning an organization-scoped user (the device-connect prerequisite) and
// fetching a user's per-metric data, plus the organization access-token auth that
// gates every call.
//
// CONFIRMED from the public Inform REST API docs:
//   * base URL   https://api.v2.validic.com
//   * auth       token-based: the Organization ID is a path segment and the
//                Organization Access Token is the `token` query parameter on
//                every request (HTTPS only).
//   * provision  POST /organizations/:org_id/users?token=:token  with body
//                {"uid": "<your_user_id>"} — returns id, uid, marketplace info
//                (incl. the per-user access/marketplace token + connect URL),
//                and created_at. Provision when a user is ready to connect.
//   * fetch      GET /organizations/:org_id/users/:uid/:object_type
//                  ?token=:token&start_date=YYYY-MM-DD&end_date=YYYY-MM-DD
//                object_type ∈ {measurements, intraday, cgm, nutrition, sleep,
//                summaries, workouts}. 30-day max window per request.
//
// Config mapping: api_key = Organization Access Token (the `token` param);
// client_id = Organization ID (the :org_id path segment). base_url defaults to
// the confirmed public host.
//
// NOT IMPLEMENTED (left as inherited stubs — contracts not in the public
// reference, so fabricating them would be worse than an honest "not implemented"):
//   * the EHR write-back into Epic/Cerner — Validic's headline moat, but no
//     public POST/PUT write-back endpoint is documented;
//   * authorize_url / list_providers — the consumer marketplace connect URL is
//     returned by provisioning, not via a documented authorize endpoint;
//   * handle_webhook (Inform Streaming API) and revoke (no documented user-delete
//     endpoint in the public reference).

#include "health/vendor/vendor.hpp"

#include "client/http_client.hpp"

#include <cctype>
#include <string>
#include <utility>

namespace mirobody { namespace vendor {
namespace {

//------------------------------------------------------------------------------
// Confirmed defaults (see header comment).
//------------------------------------------------------------------------------

const char kDefaultBase[] = "https://api.v2.validic.com/";

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

// JSON-string-escape a value for the small provision body we build by hand.
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

class Validic : public VendorBase {
public:
    explicit Validic(VendorConfig cfg) : VendorBase(make_info(), std::move(cfg)) {}

    // Fetch `domain` for `user_id` over [start_iso, end_iso]. `user_id` is the
    // organization-scoped `uid`; the domain selects the Inform metric object
    // type. The Organization ID (client_id) and Organization Access Token
    // (api_key) authenticate the request. Empty bounds => Validic's default
    // window (params omitted; defaults to the current UTC day). Returns the
    // response JSON verbatim. Note Validic accepts YYYY-MM-DD dates; ISO-8601
    // bounds are passed through as given.
    std::string fetch(const std::string& user_id,
                      DataDomain domain,
                      const std::string& start_iso,
                      const std::string& end_iso) override {
        require_configured();

        const std::string uid(user_id);
        if (uid.empty()) {
            throw VendorError(info_.id + ": fetch requires a user_id (the organization-scoped uid)");
        }

        const char* object_type = metric_object_type(domain);
        if (!object_type) {
            throw VendorError(info_.id + ": no Inform metric object type for domain '" +
                              to_string(domain) + "'");
        }

        // GET /organizations/:org_id/users/:uid/:object_type?token=...&start_date=...&end_date=...
        std::string url = base_url() + "organizations/" + url_encode(org_id()) +
                          "/users/" + url_encode(uid) + "/" + object_type +
                          "?token=" + url_encode(access_token());
        if (!start_iso.empty()) {
            url += "&start_date=" + url_encode(std::string(start_iso));
        }
        if (!end_iso.empty()) {
            url += "&end_date=" + url_encode(std::string(end_iso));
        }

        client::HttpResponse res = client::HttpClient().get(url, /*timeout_ms=*/30000);
        if (res.status < 200 || res.status >= 300) {
            throw VendorError(http_error(std::string("fetch ") + object_type, res));
        }
        return res.body;
    }

    // Provision an organization-scoped user so a device can be connected. `uid`
    // is the caller-defined identifier Validic ties the user to. Returns the
    // creation response JSON verbatim (id, uid, marketplace info incl. the
    // per-user access/marketplace token + connect URL, created_at).
    //
    // Not on the Vendor interface; reachable through the concrete type. The
    // consent flow proper (authorize_url) stays a stub because the connect URL
    // is delivered in this response rather than via a documented authorize
    // endpoint.
    std::string provision_user(const std::string& uid) {
        require_configured();
        const std::string user(uid);
        if (user.empty()) {
            throw VendorError(info_.id + ": provision_user requires a uid");
        }

        client::HttpRequest req;
        req.url  = base_url() + "organizations/" + url_encode(org_id()) +
                   "/users?token=" + url_encode(access_token());
        req.body = std::string("{\"uid\":\"") + json_escape(user) + "\"}";

        client::HttpResponse res = client::HttpClient().post(req);
        if (res.status < 200 || res.status >= 300) {
            throw VendorError(http_error("provision_user (organizations/:org_id/users)", res));
        }
        return res.body;
    }

private:
    //--------------------------------------------------------------------------
    // Domain → Inform metric object type (confirmed object_type values)
    //--------------------------------------------------------------------------

    static const char* metric_object_type(DataDomain d) {
        switch (d) {
            // BP cuffs, pulse oximeters, weight, etc. — Validic's core PGHD.
            case DataDomain::BodyMetrics: return "measurements";
            // Continuous glucose monitoring stream.
            case DataDomain::Glucose:     return "cgm";
            // Activity / heart-rate detail is exposed via the intraday object.
            case DataDomain::Activity:    return "intraday";
            case DataDomain::HeartRate:   return "intraday";
            case DataDomain::Sleep:       return "sleep";
            case DataDomain::Nutrition:   return "nutrition";
            // Clinical (EHR) data has no public REST fetch object type.
            default:                      return nullptr;
        }
    }

    //--------------------------------------------------------------------------
    // Auth + config
    //--------------------------------------------------------------------------

    void require_configured() const {
        if (config().api_key.empty() || config().client_id.empty()) {
            throw VendorError(info_.id + ": not configured — set "
                              "MIROBODY_VENDOR_VALIDIC_API_KEY (Organization Access Token) and "
                              "MIROBODY_VENDOR_VALIDIC_CLIENT_ID (Organization ID)");
        }
    }

    // Organization Access Token, sent as the `token` query parameter.
    const std::string& access_token() const { return config().api_key; }

    // Organization ID, used as the :org_id path segment.
    const std::string& org_id() const { return config().client_id; }

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
        i.id                   = "validic";
        i.display_name         = "Validic";
        i.positioning          = "Enterprise PGHD infrastructure";
        i.target_customers     = "Health systems, health plans/insurers, wellness";
        i.data_source_coverage = "700+ health devices (BP cuffs, pulse oximeters, and other PGHD)";
        i.integration_method   = "REST API, iOS/Android SDK, direct EHR write-back";
        i.compliance_summary   = "HIPAA, SOC 2 Type II";
        i.differentiator       = "Deep integration with EHRs like Epic/Cerner; Validic Inform is now free";
        i.docs_url             = "https://validic.com";
        i.region               = Region::US;
        i.open_source          = false;
        i.compliance           = {"HIPAA", "SOC2_TYPE_II"};
        i.domains              = {DataDomain::BodyMetrics, DataDomain::HeartRate,
                                  DataDomain::Glucose, DataDomain::Clinical};
        i.integrations         = {Integration::Rest, Integration::Sdk};
        if (i.docs_url.empty()) {
            i.docs_url = "https://developer.validic.com";
        }
        return i;
    }
};

}

std::unique_ptr<Vendor> make_validic(const VendorConfig& cfg) {
    return std::unique_ptr<Vendor>(new Validic(cfg));
}

}}

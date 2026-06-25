// Thryve — European medical-grade wearable API. See README.md (Segment C, EU).
//
// Implemented against the public reference at https://docs.thryve.health — a
// GDPR-first, EU-hosted wearable aggregation API (500+ devices, 250+ metrics)
// exposing a "unified data format" over two timeseries endpoints: daily
// aggregates and intraday ("epoch") values. This client covers the lifecycle a
// server needs: create an end user (the access token that *is* the user id),
// hand off to the hosted connection widget for consent, fetch unified daily /
// epoch data, and delete the user on revoke.
//
// CONFIRMED from the public docs (https://docs.thryve.health):
//   * base host    https://api.thryve.de  (EU-hosted, per the GDPR positioning)
//   * auth         TWO HTTP Basic headers on every request —
//                    Authorization:    Basic base64(username:password)
//                    AppAuthorization: Basic base64(authID:authSecret)
//                  We map client_id:client_secret -> Authorization and
//                  api_key (formatted "authID:authSecret") -> AppAuthorization.
//   * create user  POST /v5/accessToken  -> plain-text access token == endUserId
//   * connect      POST /widget/v6/connection  (JSON {endUserId, locale}) ->
//                  hosted Connection Widget URL to redirect the user to
//   * daily data   POST /v5/dailyDynamicValues  (x-www-form-urlencoded;
//                    authenticationToken + startDay/endDay or *Unix range)
//   * epoch data   POST /v5/dynamicEpochValues  (x-www-form-urlencoded;
//                    authenticationToken + startTimestamp/endTimestamp range)
//   * revoke       DELETE /v5/userInformation  (deletes the Thryve user)
// Request bodies are application/x-www-form-urlencoded and the documented form
// keys (authenticationToken, startDay/endDay, startTimestamp/endTimestamp,
// dataSources, valueTypes) are taken verbatim from the reference.
//
// INFERRED — the numeric Thryve DataType IDs used to filter by metric are only
// partially published, so per-domain valueTypes filtering is centralized in the
// constants below and applied only where the IDs are confirmed; for domains
// without a confirmed ID (e.g. glucose) we omit the filter and return the full
// unified payload for the window rather than guess an ID. The credential->header
// split (client pair -> Authorization, api_key -> AppAuthorization) is the one
// mapping choice not spelled out by the docs and is flagged here too.

#include "health/vendor/vendor.hpp"

#include "client/http_client.hpp"

#include <rapidjson/document.h>
#include <rapidjson/stringbuffer.h>
#include <rapidjson/writer.h>

#include <cctype>
#include <string>
#include <utility>
#include <vector>

namespace mirobody { namespace vendor {
namespace {

//------------------------------------------------------------------------------
// Confirmed defaults + form-field names (see header comment).
//------------------------------------------------------------------------------

const char kDefaultBase[] = "https://api.thryve.de/";

// Confirmed endpoint paths (relative to the base host).
const char kAccessTokenPath[] = "v5/accessToken";
const char kWidgetPath[]      = "widget/v6/connection";
const char kDailyPath[]       = "v5/dailyDynamicValues";
const char kEpochPath[]       = "v5/dynamicEpochValues";
const char kUserInfoPath[]    = "v5/userInformation";

// Confirmed form keys for the data endpoints.
const char kTokenField[]     = "authenticationToken";
const char kStartDayField[]  = "startDay";          // daily, ISO date
const char kEndDayField[]    = "endDay";            // daily, ISO date
const char kStartTsField[]   = "startTimestamp";    // epoch, ISO 8601
const char kEndTsField[]     = "endTimestamp";      // epoch, ISO 8601
const char kValueTypesField[]= "valueTypes";        // comma-separated DataType IDs

// INFERRED: Thryve numeric DataType IDs per domain. Only the confirmed ones are
// listed; an empty string means "do not filter" (return the full unified set for
// the window). Centralized so they can be reconciled against the (partly gated)
// biomarker reference without touching the fetch logic.
const char kValueTypesActivity[]  = "1000";          // Steps (confirmed)
const char kValueTypesSleep[]     = "";              // multiple sleep IDs; left unfiltered
const char kValueTypesHeartRate[] = "3000";          // HeartRate epoch (confirmed)
const char kValueTypesGlucose[]   = "";              // numeric ID not publicly confirmed

//------------------------------------------------------------------------------
// Local helpers
//------------------------------------------------------------------------------

// Percent-encode a query-string / form component (RFC 3986 unreserved pass
// through). Local copy, matching user/service.cpp, vitalera.cpp, healthconnect.cpp.
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

class Thryve : public VendorBase {
public:
    explicit Thryve(VendorConfig cfg) : VendorBase(make_info(), std::move(cfg)) {}

    // Begin consent: ask Thryve for a hosted Connection Widget URL for this user
    // and return it for the caller to redirect to. `state` has no documented slot
    // in the v6 widget request, so it is left to the caller's redirect handling;
    // `redirect_uri` is likewise configured on the Thryve app side. We pass the
    // endUserId (Thryve's user identifier == the access token from createUser).
    std::string authorize_url(const std::string& redirect_uri,
                              const std::string& /*state*/) override {
        require_configured();
        const std::string uid(redirect_uri);  // caller passes the endUserId here
        if (uid.empty()) {
            throw VendorError(info_.id + ": authorize_url requires the endUserId "
                              "(create one first via createUser / POST v5/accessToken)");
        }

        rapidjson::StringBuffer sb;
        rapidjson::Writer<rapidjson::StringBuffer> w(sb);
        w.StartObject();
        w.Key("endUserId");
        w.String(uid.c_str());
        w.EndObject();

        client::HttpRequest req;
        req.url          = base_url() + kWidgetPath;
        req.content_type = "application/json";
        req.body         = sb.GetString();
        req.headers      = auth_headers();
        client::HttpResponse res = client::HttpClient().post(req);
        if (res.status < 200 || res.status >= 300) {
            throw VendorError(http_error("authorize_url (widget/v6/connection)", res));
        }
        // The widget response is JSON carrying the (expiring) widget URL; return
        // it verbatim so the caller can extract whichever URL field it needs.
        return res.body;
    }

    // Fetch `domain` for `user_id` over [start_iso, end_iso] in Thryve's unified
    // data format. `user_id` is the endUserId / authenticationToken returned by
    // createUser. Heart-rate uses the intraday "epoch" endpoint; the remaining
    // domains use the daily-aggregate endpoint. Empty bounds omit the range
    // filter (Thryve applies its default window). Returns the JSON verbatim.
    std::string fetch(const std::string& user_id,
                      DataDomain domain,
                      const std::string& start_iso,
                      const std::string& end_iso) override {
        require_configured();
        const std::string uid(user_id);
        if (uid.empty()) {
            throw VendorError(info_.id + ": fetch requires a user_id (the endUserId / "
                              "authenticationToken from createUser)");
        }

        if (domain == DataDomain::HeartRate) {
            return fetch_epoch(uid, domain, start_iso, end_iso);
        }
        return fetch_daily(uid, domain, start_iso, end_iso);
    }

    // Disconnect the user: delete the Thryve user (and thereby their connections)
    // via DELETE /v5/userInformation, scoped by the authenticationToken.
    void revoke(const std::string& user_id) override {
        require_configured();
        const std::string uid(user_id);
        if (uid.empty()) {
            throw VendorError(info_.id + ": revoke requires a user_id (the endUserId)");
        }
        client::HttpRequest req;
        req.url          = base_url() + kUserInfoPath;
        req.content_type = "application/x-www-form-urlencoded";
        req.body         = std::string(kTokenField) + "=" + url_encode(uid);
        req.headers      = auth_headers();
        client::HttpResponse res = client::HttpClient().request("DELETE", req);
        if (res.status < 200 || res.status >= 300) {
            throw VendorError(http_error("revoke (v5/userInformation)", res));
        }
    }

private:
    //--------------------------------------------------------------------------
    // Operations
    //--------------------------------------------------------------------------

    // Daily aggregates: POST /v5/dailyDynamicValues with ISO `startDay`/`endDay`.
    std::string fetch_daily(const std::string& uid,
                            DataDomain domain,
                            const std::string& start_iso,
                            const std::string& end_iso) {
        std::string form = std::string(kTokenField) + "=" + url_encode(uid);
        if (!start_iso.empty()) {
            form += std::string("&") + kStartDayField + "=" + url_encode(std::string(start_iso));
        }
        if (!end_iso.empty()) {
            form += std::string("&") + kEndDayField + "=" + url_encode(std::string(end_iso));
        }
        append_value_types(form, domain);
        return post_form(kDailyPath, form, "fetch v5/dailyDynamicValues");
    }

    // Intraday epochs: POST /v5/dynamicEpochValues with ISO-8601
    // `startTimestamp`/`endTimestamp`.
    std::string fetch_epoch(const std::string& uid,
                            DataDomain domain,
                            const std::string& start_iso,
                            const std::string& end_iso) {
        std::string form = std::string(kTokenField) + "=" + url_encode(uid);
        if (!start_iso.empty()) {
            form += std::string("&") + kStartTsField + "=" + url_encode(std::string(start_iso));
        }
        if (!end_iso.empty()) {
            form += std::string("&") + kEndTsField + "=" + url_encode(std::string(end_iso));
        }
        append_value_types(form, domain);
        return post_form(kEpochPath, form, "fetch v5/dynamicEpochValues");
    }

    // Append the inferred valueTypes filter for `domain`, if a confirmed ID set
    // exists; otherwise leave the request unfiltered (full unified payload).
    static void append_value_types(std::string& form, DataDomain domain) {
        const char* vt = value_types(domain);
        if (vt && vt[0] != '\0') {
            form += std::string("&") + kValueTypesField + "=" + url_encode(vt);
        }
    }

    static const char* value_types(DataDomain d) {
        switch (d) {
            case DataDomain::Activity:  return kValueTypesActivity;
            case DataDomain::Sleep:     return kValueTypesSleep;
            case DataDomain::HeartRate: return kValueTypesHeartRate;
            case DataDomain::Glucose:   return kValueTypesGlucose;
            default:                    return "";   // unfiltered
        }
    }

    //--------------------------------------------------------------------------
    // Transport + auth
    //--------------------------------------------------------------------------

    std::string post_form(const char* path, const std::string& form, const std::string& op) {
        client::HttpRequest req;
        req.url          = base_url() + path;
        req.content_type = "application/x-www-form-urlencoded";
        req.body         = form;
        req.headers      = auth_headers();
        client::HttpResponse res = client::HttpClient().post(req);
        if (res.status < 200 || res.status >= 300) {
            throw VendorError(http_error(op, res));
        }
        return res.body;
    }

    // Both Thryve auth headers. The client pair is the partner/user credential
    // (Authorization); the api_key carries the app credential "authID:authSecret"
    // (AppAuthorization). Each is sent as HTTP Basic over base64. The split is the
    // one inferred mapping (see header comment); the header names are confirmed.
    std::vector<std::string> auth_headers() const {
        std::vector<std::string> h;
        if (!config().client_id.empty() || !config().client_secret.empty()) {
            h.push_back("Authorization: Basic " +
                        base64(config().client_id + ":" + config().client_secret));
        }
        if (!config().api_key.empty()) {
            // api_key is expected formatted as "authID:authSecret".
            h.push_back("AppAuthorization: Basic " + base64(config().api_key));
        }
        return h;
    }

    // Minimal RFC 4648 base64 (no line breaks) for the Basic credentials.
    static std::string base64(const std::string& in) {
        static const char tbl[] =
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
        const std::size_t rem = in.size() - i;
        if (rem == 1) {
            unsigned n = static_cast<unsigned char>(in[i]) << 16;
            out.push_back(tbl[(n >> 18) & 0x3F]);
            out.push_back(tbl[(n >> 12) & 0x3F]);
            out.push_back('=');
            out.push_back('=');
        } else if (rem == 2) {
            unsigned n = (static_cast<unsigned char>(in[i]) << 16) |
                         (static_cast<unsigned char>(in[i + 1]) << 8);
            out.push_back(tbl[(n >> 18) & 0x3F]);
            out.push_back(tbl[(n >> 12) & 0x3F]);
            out.push_back(tbl[(n >> 6) & 0x3F]);
            out.push_back('=');
        }
        return out;
    }

    //--------------------------------------------------------------------------
    // Misc
    //--------------------------------------------------------------------------

    void require_configured() const {
        if (!config().configured()) {
            throw VendorError(info_.id + ": not configured — set "
                              "MIROBODY_VENDOR_THRYVE_API_KEY (the app credential "
                              "\"authID:authSecret\" for the AppAuthorization header) "
                              "and/or _CLIENT_ID/_CLIENT_SECRET (the Authorization "
                              "credential)");
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
        i.id                   = "thryve";
        i.display_name         = "Thryve";
        i.positioning          = "European medical-grade wearable API";
        i.target_customers     = "Insurers, digital health, clinical trials, pharma";
        i.data_source_coverage = "500+ devices, 250+ metrics (incl. cardiovascular, diabetes)";
        i.integration_method   = "Plug-and-play API, unified data format";
        i.compliance_summary   = "GDPR, HIPAA, ISO 9001/27001";
        i.differentiator       = "Fully developed and hosted in Europe; real-time trends and risk-prediction models";
        i.docs_url             = "https://www.thryve.health";
        i.region               = Region::EU;
        i.open_source          = false;
        i.compliance           = {"GDPR", "HIPAA", "ISO_9001", "ISO_27001"};
        i.domains              = {DataDomain::Activity, DataDomain::Sleep, DataDomain::HeartRate,
                                  DataDomain::Glucose};
        i.integrations         = {Integration::Rest};
        return i;
    }
};

}

std::unique_ptr<Vendor> make_thryve(const VendorConfig& cfg) {
    return std::unique_ptr<Vendor>(new Thryve(cfg));
}

}}

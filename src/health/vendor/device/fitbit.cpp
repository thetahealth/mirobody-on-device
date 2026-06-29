// Fitbit (Google) — direct client for a consumer wearable brand. Brands like
// this are normally reached through an aggregator (Terra/Validic/…); this is a
// direct integration for deployments that want to talk to Fitbit's public Web
// API without one. See src/health/README.md.
//
// Fitbit Web API (https://dev.fitbit.com/build/reference/web-api/):
//   * base_url defaults to the documented host https://api.fitbit.com
//     (override via MIROBODY_VENDOR_FITBIT_BASE_URL).
//   * auth is an OAuth 2.0 access token (config.api_key) as a Bearer header on
//     fetch(); the token is exchanged out of band at /oauth2/token.
//   * authorize_url() builds the standard authorization-code consent URL at
//     www.fitbit.com/oauth2/authorize from config.client_id (no network call; a
//     confidential server client authenticates with its secret at the token step,
//     so PKCE is not used). revoke() POSTs the token to /oauth2/revoke with HTTP
//     Basic client_id:client_secret. handle_webhook() verifies the subscription
//     notification's X-Fitbit-Signature (base64(HMAC-SHA1(raw_body)) keyed by the
//     client secret + "&"). All confirmed at dev.fitbit.com/build/reference/web-api.
//   * fetch() issues the documented per-domain time-series GETs over a
//     [startDate, endDate] day range (yyyy-MM-dd). The user path segment is the
//     Fitbit user id, or "-" (the API's alias for the token's own user).

#include "health/vendor/vendor.hpp"

#include "client/http_client.hpp"
#include "storage/sign.hpp"   // hmac_sha1 / base64_encode

#include <rapidjson/document.h>

#include <array>
#include <cctype>
#include <cstddef>
#include <string>
#include <utility>
#include <vector>

namespace mirobody { namespace vendor {
namespace {

// OAuth endpoints. Note authorize lives on www.fitbit.com; the token/revoke/API
// endpoints live on api.fitbit.com (base_url()).
const char kAuthorizeUrl[] = "https://www.fitbit.com/oauth2/authorize";
// Consent scopes matching the domains this client brokers (space-separated per
// the docs; url_encode turns the spaces into %20).
const char kDefaultScope[] = "activity heartrate sleep weight profile";

// yyyy-MM-dd prefix of an ISO-8601 timestamp, or "" if it does not start with a
// date. Fitbit time-series endpoints take whole days, not instants.
std::string ymd(const std::string& iso) {
    if (iso.size() >= 10 &&
        std::isdigit(static_cast<unsigned char>(iso[0])) && iso[4] == '-' && iso[7] == '-') {
        return iso.substr(0, 10);
    }
    return std::string();
}

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

// Constant-time compare (length is not secret) for webhook signatures.
bool ct_equal(const std::string& a, const std::string& b) {
    if (a.size() != b.size()) return false;
    unsigned char diff = 0;
    for (std::size_t i = 0; i < a.size(); ++i) {
        diff |= static_cast<unsigned char>(a[i]) ^ static_cast<unsigned char>(b[i]);
    }
    return diff == 0;
}

// Find a header value in a raw "Name: value\r\n..." block, case-insensitive name.
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

class Fitbit : public VendorBase {
public:
    explicit Fitbit(VendorConfig cfg) : VendorBase(make_info(), std::move(cfg)) {}

    // Fetch `domain` for `user_id` over [start_iso, end_iso] as the Fitbit JSON.
    // `user_id` is the Fitbit user id; empty falls back to "-" (the token owner).
    std::string fetch(const std::string& user_id,
                      DataDomain domain,
                      const std::string& start_iso,
                      const std::string& end_iso) override {
        require_token();
        const std::string base = base_url();
        const std::string u = user_id.empty() ? std::string("-") : user_id;

        const std::string s = ymd(start_iso);
        const std::string e = ymd(end_iso);
        if (s.empty() || e.empty()) {
            throw VendorError(info_.id + ": fetch requires ISO-8601 start/end dates (yyyy-MM-dd day range)");
        }

        const std::string url = base + endpoint(domain, u, s, e);
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

    // Build the OAuth 2.0 authorization-code consent URL the user is redirected to
    // (pure construction; the token is exchanged out of band at /oauth2/token).
    // `redirect_uri`/`state` are the standard OAuth params; user_id/provider are
    // unused (Fitbit is a single brand; the user is identified by the resulting
    // token). Requires the OAuth client_id. Scopes default to the brokered domains.
    std::string authorize_url(const std::string& redirect_uri,
                              const std::string& state,
                              const std::string& /*user_id*/,
                              const std::string& /*provider*/) override {
        if (config().client_id.empty()) {
            throw VendorError(info_.id + ": authorize_url requires an OAuth client_id — set "
                              "MIROBODY_VENDOR_FITBIT_CLIENT_ID");
        }
        std::string url = std::string(kAuthorizeUrl) +
                          "?response_type=code&client_id=" + url_encode(config().client_id) +
                          "&scope=" + url_encode(kDefaultScope);
        if (!redirect_uri.empty()) {
            url += "&redirect_uri=" + url_encode(std::string(redirect_uri));
        }
        if (!state.empty()) {
            url += "&state=" + url_encode(std::string(state));
        }
        return url;
    }

    // Revoke the user's authorization: POST /oauth2/revoke with the access token,
    // authenticated as the confidential client (HTTP Basic client_id:client_secret).
    // Revoking either token type drops the whole grant. The token revoked is the
    // configured access token (config.api_key); user_id/provider are unused.
    void revoke(const std::string& /*user_id*/, const std::string& /*provider*/) override {
        require_token();
        if (config().client_id.empty() || config().client_secret.empty()) {
            throw VendorError(info_.id + ": revoke requires OAuth client_id + client_secret for "
                              "Basic auth on /oauth2/revoke — set MIROBODY_VENDOR_FITBIT_CLIENT_ID "
                              "and _CLIENT_SECRET");
        }
        client::HttpRequest req;
        req.url          = base_url() + "/oauth2/revoke";
        req.body         = "token=" + url_encode(config().api_key);
        req.content_type = "application/x-www-form-urlencoded";
        req.headers      = {
            "Authorization: Basic " +
                storage::base64_encode(config().client_id + ":" + config().client_secret),
        };
        client::HttpResponse res = client::HttpClient().post(req);
        if (res.status < 200 || res.status >= 300) {
            throw VendorError(info_.id + ": revoke failed (HTTP " +
                              std::to_string(res.status) + "): " + res.body.substr(0, 300));
        }
    }

    // Verify and return an inbound Fitbit subscription notification. The POST body
    // is a JSON array; its signature is in the `X-Fitbit-Signature` header as
    // base64(HMAC-SHA1(raw_body)) keyed by the OAuth client secret with '&'
    // appended (config.client_secret + "&"). On success returns the body verbatim.
    //
    // Note: Fitbit also verifies a new subscriber endpoint with a GET carrying a
    // `verify` query param (respond 204 for the dashboard's code, 404 otherwise) —
    // that handshake is the HTTP layer's job; this method handles the signed POST.
    std::string handle_webhook(const std::string& raw_headers,
                               const std::string& body) override {
        if (config().client_secret.empty()) {
            throw VendorError(info_.id + ": handle_webhook requires the OAuth client_secret "
                              "(the webhook signing key) — set MIROBODY_VENDOR_FITBIT_CLIENT_SECRET");
        }
        const std::string sig = header_value(raw_headers, "x-fitbit-signature");
        if (sig.empty()) {
            throw VendorError(info_.id + ": webhook missing X-Fitbit-Signature header");
        }
        const std::string key = config().client_secret + "&";
        std::array<unsigned char, 20> mac = storage::hmac_sha1(key, std::string(body));
        const std::string expected = storage::base64_encode(mac.data(), mac.size());
        if (!ct_equal(expected, sig)) {
            throw VendorError(info_.id + ": webhook signature verification failed (X-Fitbit-Signature)");
        }
        rapidjson::Document doc;
        if (doc.Parse(std::string(body).c_str()).HasParseError() ||
            !(doc.IsArray() || doc.IsObject())) {
            throw VendorError(info_.id + ": webhook body was not valid JSON");
        }
        return std::string(body);
    }

private:
    // Documented Web API time-series paths. Sleep is served by API v1.2; the rest
    // by v1. Throws for domains Fitbit does not broker.
    static std::string endpoint(DataDomain d, const std::string& u,
                                const std::string& s, const std::string& e) {
        switch (d) {
            case DataDomain::Activity:
                return "/1/user/" + u + "/activities/steps/date/" + s + "/" + e + ".json";
            case DataDomain::HeartRate:
                return "/1/user/" + u + "/activities/heart/date/" + s + "/" + e + ".json";
            case DataDomain::Sleep:
                return "/1.2/user/" + u + "/sleep/date/" + s + "/" + e + ".json";
            case DataDomain::BodyMetrics:
                return "/1/user/" + u + "/body/log/weight/date/" + s + "/" + e + ".json";
            default:
                throw VendorError(std::string("fitbit: unsupported domain '") + to_string(d) +
                                  "' (Fitbit brokers: activity, heart_rate, sleep, body_metrics)");
        }
    }

    std::string base_url() const {
        std::string b = config().base_url;
        if (b.empty()) b = "https://api.fitbit.com";
        while (!b.empty() && b.back() == '/') b.pop_back();
        return b;
    }

    void require_token() const {
        if (config().api_key.empty()) {
            throw VendorError(info_.id + ": api_key is required — set "
                              "MIROBODY_VENDOR_FITBIT_API_KEY to a Fitbit OAuth 2.0 access token");
        }
    }

    static VendorInfo make_info() {
        VendorInfo i;
        i.id                   = "fitbit";
        i.display_name         = "Fitbit";
        i.positioning          = "Consumer wearable brand (Google) with a public OAuth2 Web API";
        i.target_customers     = "Apps serving Fitbit tracker / smartwatch users";
        i.data_source_coverage = "Fitbit trackers and smartwatches";
        i.integration_method   = "REST Web API (OAuth2) + subscription webhooks";
        i.compliance_summary   = "User-consented OAuth scopes";
        i.differentiator       = "Mature, public, well-documented consumer wearable API";
        i.docs_url             = "https://dev.fitbit.com/build/reference/web-api/";
        i.region               = Region::Global;
        i.open_source          = false;
        i.domains              = {DataDomain::Activity, DataDomain::HeartRate,
                                  DataDomain::Sleep, DataDomain::BodyMetrics};
        i.integrations         = {Integration::Rest, Integration::Webhook};
        return i;
    }
};

}

std::unique_ptr<Vendor> make_fitbit(const VendorConfig& cfg) {
    return std::unique_ptr<Vendor>(new Fitbit(cfg));
}

}}

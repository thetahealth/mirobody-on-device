// Terra API — all-scenario unified fitness & health API. See README.md (Segment A).
//
// Implemented against the public reference at https://docs.tryterra.co — 500+
// devices, 5,000+ metrics, REST + multi-language SDK + real-time WebSocket
// streams + webhooks. This client covers the three operations the platform leads
// with: the Terra Connect widget session (consent flow), the per-resource
// historical-data fetch endpoints, and inbound webhook signature verification.
//
// CONFIRMED from the public docs (docs.tryterra.co):
//   * base URL        https://api.tryterra.co/v2
//   * auth            two headers on every call: `dev-id` and `x-api-key`
//                     (plus Content-Type: application/json on POSTs)
//   * widget session  POST /v2/auth/generateWidgetSession with a JSON body of
//                     { reference_id, language, auth_success_redirect_url,
//                       auth_failure_redirect_url }; response carries the widget
//                     link in the "url" field (+ session_id, status, expires_in)
//   * data fetch      GET /v2/{resource} where resource is one of
//                     activity / sleep / body / daily / menstruation / nutrition,
//                     with query params user_id (required, Terra UUID),
//                     start_date (required), end_date (optional), and
//                     to_webhook=false to receive the data inline in the response
//   * webhooks        terra-signature header is "t=<unix>,v1=<hex>"; v1 is the
//                     hex HMAC-SHA256 of the signed payload "<t>.<raw_body>"
//                     keyed by the endpoint's signing secret. Verification uses a
//                     constant-time compare of the recomputed digest.
//
// CONFIG MAPPING (see VendorConfig): Terra splits its credentials across three
// fields, which we map onto VendorConfig as:
//   * client_id     -> dev-id        (developer id, sent on every request)
//   * api_key       -> x-api-key     (API key, sent on every request)
//   * client_secret -> signing secret (webhook HMAC key; only handle_webhook
//                       needs it — the dashboard's per-destination signing secret)
// This mapping is an INFERRED local convention (the docs name the three secrets
// dev-id / x-api-key / signing-secret; VendorConfig has no dedicated slot for the
// last, so it rides on client_secret). The header *names* and the secrets they
// carry are confirmed; only which VendorConfig field holds the signing secret is
// our choice. list_providers and revoke stay inherited stubs: list_providers has
// no single documented "list everything" REST contract here, and Terra
// deauthentication is keyed by Terra user_id via a path we did not confirm.

#include "health/vendor/vendor.hpp"

#include "client/http_client.hpp"

#include <openssl/hmac.h>
#include <openssl/crypto.h>

#include <rapidjson/document.h>

#include <cctype>
#include <cstddef>
#include <string>
#include <utility>
#include <vector>

namespace mirobody { namespace vendor {
namespace {

//------------------------------------------------------------------------------
// Confirmed defaults (see header comment).
//------------------------------------------------------------------------------

const char kDefaultBase[] = "https://api.tryterra.co/v2";

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

// JSON-string-escape a value for the small widget-session body we build by hand
// (avoids pulling in a Writer for four fields). Matches vitalera.cpp.
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

// HMAC-SHA256 of `msg` under `key`, returned as lowercase hex (the form Terra's
// terra-signature `v1=` component is encoded in).
std::string hmac_sha256_hex(const std::string& key, const std::string& msg) {
    unsigned char raw[EVP_MAX_MD_SIZE];
    unsigned int raw_len = 0;
    HMAC(EVP_sha256(),
         key.data(), static_cast<int>(key.size()),
         reinterpret_cast<const unsigned char*>(msg.data()), msg.size(),
         raw, &raw_len);

    static const char* hex = "0123456789abcdef";
    std::string out;
    out.reserve(raw_len * 2);
    for (unsigned int i = 0; i < raw_len; ++i) {
        out.push_back(hex[raw[i] >> 4]);
        out.push_back(hex[raw[i] & 0x0F]);
    }
    return out;
}

// Constant-time compare of two equal-length strings (length itself is not
// secret). Mirrors jwt.cpp's ct_equal.
bool ct_equal(const std::string& a, const std::string& b) {
    if (a.size() != b.size()) return false;
    if (a.empty()) return true;
    return CRYPTO_memcmp(a.data(), b.data(), a.size()) == 0;
}

//------------------------------------------------------------------------------

class Terra : public VendorBase {
public:
    explicit Terra(VendorConfig cfg) : VendorBase(make_info(), std::move(cfg)) {}

    // Begin consent via the Terra Connect widget. POSTs to
    // /auth/generateWidgetSession and returns the widget `url` the caller
    // redirects the user to. `state` rides along as Terra's `reference_id` (the
    // developer's own end-user id, echoed back) for correlation; `redirect_uri`
    // is used as the success redirect, with the failure redirect pointed at the
    // same URL (callers can disambiguate via Terra's appended query params).
    std::string authorize_url(const std::string& redirect_uri,
                              const std::string& state) override {
        require_configured();

        const std::string redirect(redirect_uri);
        const std::string reference(state);

        std::string body = "{";
        bool first = true;
        auto add_field = [&](const char* k, const std::string& v) {
            if (v.empty()) return;
            if (!first) body += ",";
            first = false;
            body += "\"";
            body += k;
            body += "\":\"";
            body += json_escape(v);
            body += "\"";
        };
        add_field("reference_id", reference);
        if (!redirect.empty()) {
            add_field("auth_success_redirect_url", redirect);
            add_field("auth_failure_redirect_url", redirect);
        }
        body += "}";

        client::HttpRequest req;
        req.url     = base_url() + "/auth/generateWidgetSession";
        req.body    = body;
        req.headers = auth_headers();
        client::HttpResponse res = client::HttpClient().post(req);
        if (res.status < 200 || res.status >= 300) {
            throw VendorError(http_error("authorize_url (auth/generateWidgetSession)", res));
        }

        rapidjson::Document doc;
        if (doc.Parse(res.body.c_str()).HasParseError() || !doc.IsObject()) {
            throw VendorError(info_.id + ": generateWidgetSession response was not a JSON object");
        }
        if (!doc.HasMember("url") || !doc["url"].IsString()) {
            throw VendorError(info_.id + ": no widget 'url' in generateWidgetSession response");
        }
        return doc["url"].GetString();
    }

    // Fetch `domain` for `user_id` over [start_iso, end_iso]. `user_id` is the
    // Terra user UUID returned by the consent flow. Maps the domain to a Terra
    // resource endpoint; start_date is required by Terra (we surface a clear
    // error if absent), end_date is optional. to_webhook=false asks Terra to
    // return the payload inline rather than pushing it to the webhook. Returns
    // the response JSON verbatim.
    std::string fetch(const std::string& user_id,
                      DataDomain domain,
                      const std::string& start_iso,
                      const std::string& end_iso) override {
        require_configured();

        const std::string uid(user_id);
        if (uid.empty()) {
            throw VendorError(info_.id + ": fetch requires a user_id (the Terra user UUID)");
        }

        const char* resource = terra_resource(domain);
        if (!resource) {
            throw VendorError(info_.id + ": no Terra resource endpoint for domain '" +
                              to_string(domain) + "'");
        }

        const std::string start(start_iso);
        if (start.empty()) {
            throw VendorError(info_.id + ": fetch requires a start_iso (Terra's start_date is "
                              "mandatory; ISO-8601 YYYY-MM-DD or unix seconds)");
        }

        std::string url = base_url() + "/" + resource +
                          "?user_id=" + url_encode(uid) +
                          "&start_date=" + url_encode(start) +
                          "&to_webhook=false";
        if (!end_iso.empty()) {
            url += "&end_date=" + url_encode(std::string(end_iso));
        }

        client::HttpResponse res = client::HttpClient().get(url, /*timeout_ms=*/30000, auth_headers());
        if (res.status < 200 || res.status >= 300) {
            throw VendorError(http_error(std::string("fetch ") + resource, res));
        }
        return res.body;
    }

    // Verify and return an inbound Terra webhook. `raw_headers` must carry the
    // `terra-signature` header ("t=<unix>,v1=<hex>"); `body` must be the raw,
    // unaltered request body. Recomputes HMAC-SHA256 over "<t>.<body>" keyed by
    // the signing secret (config.client_secret) and constant-time compares it to
    // v1. On success returns the parsed event JSON (the body verbatim); any
    // mismatch / malformed input throws VendorError.
    std::string handle_webhook(const std::string& raw_headers,
                               const std::string& body) override {
        const std::string secret = config().client_secret;
        if (secret.empty()) {
            throw VendorError(info_.id + ": handle_webhook requires a signing secret — set "
                              "MIROBODY_VENDOR_TERRA_CLIENT_SECRET to your destination's "
                              "signing secret (from the Terra dashboard)");
        }

        std::string sig_header = find_header(raw_headers, "terra-signature");
        if (sig_header.empty()) {
            throw VendorError(info_.id + ": webhook missing terra-signature header");
        }

        // Parse "t=<timestamp>,v1=<hex>" (comma-separated prefix=value pairs).
        std::string ts;
        std::string v1;
        std::size_t start = 0;
        while (start <= sig_header.size()) {
            std::size_t comma = sig_header.find(',', start);
            std::string part = (comma == std::string::npos)
                                   ? sig_header.substr(start)
                                   : sig_header.substr(start, comma - start);
            trim(part);
            std::size_t eq = part.find('=');
            if (eq != std::string::npos) {
                std::string k = part.substr(0, eq);
                std::string v = part.substr(eq + 1);
                if (k == "t")  ts = v;
                else if (k == "v1") v1 = v;
            }
            if (comma == std::string::npos) break;
            start = comma + 1;
        }
        if (ts.empty() || v1.empty()) {
            throw VendorError(info_.id + ": malformed terra-signature header (expected t=...,v1=...)");
        }

        const std::string signed_payload = ts + "." + std::string(body);
        const std::string expected = hmac_sha256_hex(secret, signed_payload);
        if (!ct_equal(expected, v1)) {
            throw VendorError(info_.id + ": webhook signature verification failed");
        }

        rapidjson::Document doc;
        if (doc.Parse(std::string(body).c_str()).HasParseError() || !doc.IsObject()) {
            throw VendorError(info_.id + ": webhook body was not a JSON object");
        }
        return std::string(body);
    }

private:
    //--------------------------------------------------------------------------
    // Domain → Terra resource mapping (confirmed REST resource endpoints)
    //--------------------------------------------------------------------------

    static const char* terra_resource(DataDomain d) {
        switch (d) {
            case DataDomain::Activity:    return "activity";
            case DataDomain::Sleep:       return "sleep";
            // Terra surfaces heart rate within the daily summary resource.
            case DataDomain::HeartRate:   return "daily";
            case DataDomain::Glucose:     return "body";
            case DataDomain::BodyMetrics: return "body";
            default:                      return nullptr;
        }
    }

    //--------------------------------------------------------------------------
    // Transport + auth
    //--------------------------------------------------------------------------

    // Both Terra credentials go on every request, plus an explicit JSON accept.
    std::vector<std::string> auth_headers() const {
        return {
            "dev-id: " + config().client_id,
            "x-api-key: " + config().api_key,
            "Accept: application/json",
        };
    }

    void require_configured() const {
        if (config().client_id.empty() || config().api_key.empty()) {
            throw VendorError(info_.id + ": not configured — set "
                              "MIROBODY_VENDOR_TERRA_CLIENT_ID (Terra dev-id) and "
                              "MIROBODY_VENDOR_TERRA_API_KEY (Terra x-api-key)");
        }
    }

    // Base URL without a trailing slash (paths below prepend their own "/").
    std::string base_url() const {
        std::string b = config().base_url.empty() ? std::string(kDefaultBase) : config().base_url;
        while (!b.empty() && b.back() == '/') b.pop_back();
        return b;
    }

    std::string http_error(const std::string& op, const client::HttpResponse& res) const {
        // status <= 0 is a transport failure; curl puts the reason in body.
        return info_.id + ": " + op + " failed (HTTP " + std::to_string(res.status) +
               "): " + res.body.substr(0, 300);
    }

    //--------------------------------------------------------------------------
    // Header parsing for handle_webhook
    //--------------------------------------------------------------------------

    static void trim(std::string& s) {
        std::size_t b = 0;
        while (b < s.size() && std::isspace(static_cast<unsigned char>(s[b]))) ++b;
        std::size_t e = s.size();
        while (e > b && std::isspace(static_cast<unsigned char>(s[e - 1]))) --e;
        s = s.substr(b, e - b);
    }

    static char lower(char c) {
        return static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }

    // Find a header value in a raw "Name: value\r\n..." block, matching the name
    // case-insensitively (HTTP header names are case-insensitive). Returns the
    // trimmed value, or empty if absent.
    static std::string find_header(const std::string& raw_headers,
                                   const char* name) {
        const std::string blob(raw_headers);
        std::string lname(name);
        for (char& c : lname) c = lower(c);

        std::size_t line_start = 0;
        while (line_start <= blob.size()) {
            std::size_t nl = blob.find('\n', line_start);
            std::string line = (nl == std::string::npos)
                                   ? blob.substr(line_start)
                                   : blob.substr(line_start, nl - line_start);
            if (!line.empty() && line.back() == '\r') line.pop_back();

            std::size_t colon = line.find(':');
            if (colon != std::string::npos) {
                std::string key = line.substr(0, colon);
                trim(key);
                for (char& c : key) c = lower(c);
                if (key == lname) {
                    std::string val = line.substr(colon + 1);
                    trim(val);
                    return val;
                }
            }
            if (nl == std::string::npos) break;
            line_start = nl + 1;
        }
        return std::string();
    }

    //--------------------------------------------------------------------------

    static VendorInfo make_info() {
        VendorInfo i;
        i.id                   = "terra";
        i.display_name         = "Terra API";
        i.positioning          = "All-scenario unified fitness & health API";
        i.target_customers     = "Developers, clinical research, insurance, corporate wellness";
        i.data_source_coverage = "500+ devices, 5,000+ metrics (incl. CGM, menstruation, blood)";
        i.integration_method   = "REST API, multi-language SDK, WebSocket, webhook";
        i.compliance_summary   = "HIPAA, GDPR, SOC 2 Type II";
        i.differentiator       = "Real-time WebSocket data streams; built-in points/streak reward system";
        i.docs_url             = "https://tryterra.co";
        i.region               = Region::Global;
        i.open_source          = false;
        i.compliance           = {"HIPAA", "GDPR", "SOC2_TYPE_II"};
        i.domains              = {DataDomain::Activity, DataDomain::Sleep, DataDomain::HeartRate,
                                  DataDomain::Glucose, DataDomain::BodyMetrics};
        i.integrations         = {Integration::Rest, Integration::Sdk,
                                  Integration::WebSocket, Integration::Webhook};
        return i;
    }
};

}

std::unique_ptr<Vendor> make_terra(const VendorConfig& cfg) {
    return std::unique_ptr<Vendor>(new Terra(cfg));
}

}}

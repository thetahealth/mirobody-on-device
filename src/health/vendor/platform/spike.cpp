// Spike API — health data gateway + AI nutrition. See README.md (Segment A).
//
// Implemented against the public reference at https://docs.spikeapi.com — a
// health-data gateway (activity, sleep, heart rate, lab reports) with a built-in
// Nutrition AI (food-image + nutrition-label recognition) and webhook delivery.
// This client covers the operations the platform leads with: the per-user data
// queries (timeseries / sleeps / workouts / lab reports), the defining Nutrition
// AI image+label endpoints, inbound webhook verification, and disconnect via the
// provider-integration delete endpoint — all behind Spike's per-user JWT auth.
//
// CONFIRMED from the public docs (docs.spikeapi.com/api-docs/* + api-reference/*):
//   * base URL          https://app-api.spikeapi.com/v3
//   * auth              POST /auth/hmac with {application_id, application_user_id,
//                       signature}; signature = HMAC-SHA256 of application_user_id
//                       under the console shared secret. Response carries an
//                       access_token (JWT), sent as Authorization: Bearer on every
//                       subsequent call. The JWT is *per end user*, so user_id maps
//                       onto application_user_id at auth time.
//   * data fetch        GET /queries/timeseries?metric=&from_timestamp=&to_timestamp=
//                       GET /queries/sleeps?from_date=&to_date=
//                       GET /queries/workouts?from_timestamp=&to_timestamp=
//                       GET /lab_reports?from_timestamp=&to_timestamp=
//                       (timeseries/workouts/labs take UTC date-time bounds;
//                       sleeps take local-date bounds — published per-endpoint)
//   * nutrition AI      POST /nutrition_records/image            {body|body_url,...}
//                       POST /nutrition_records/ingredients/label {body|body_url,...}
//   * webhook           events POSTed as a JSON array; HMAC-SHA256 of the raw body
//                       under the shared secret, delivered in the X-Body-Signature
//                       header; endpoint acknowledges with HTTP 200.
//   * revoke            DELETE /providers/{provider_slug}/integration
//
// INFERRED — Spike documents the signature as "HMAC-SHA256 of the body using the
// shared secret" but does not pin down the wire ENCODING of that digest (hex vs
// base64). We use lowercase hex (the codebase's storage::hmac_sha256 + hex_encode
// chain, and the more common convention); it is centralized in kSigEncodeHex /
// sign_hmac() below so it can be flipped to base64 in one place if the console's
// emitted format differs. Everything else above is from the published reference.

#include "health/vendor/vendor.hpp"

#include "client/http_client.hpp"
#include "storage/sign.hpp"   // hmac_sha256 / hex_encode / base64_encode

#include <rapidjson/document.h>

#include <array>
#include <cctype>
#include <mutex>
#include <string>
#include <unordered_map>
#include <utility>

namespace mirobody { namespace vendor {
namespace {

//------------------------------------------------------------------------------
// Confirmed defaults + the one inferred signature-encoding choice (see header).
//------------------------------------------------------------------------------

const char kDefaultBase[] = "https://app-api.spikeapi.com/v3/";

// Auth request-body keys posted to /auth/hmac (confirmed field names).
const char kAuthAppIdField[]     = "application_id";
const char kAuthAppUserIdField[] = "application_user_id";
const char kAuthSignatureField[] = "signature";

// Webhook signature header (confirmed).
const char kWebhookSigHeader[] = "x-body-signature";

// Inferred: emit the HMAC digest as lowercase hex. Flip to false to switch the
// single sign_hmac() call site below to base64 if the console differs.
const bool kSigEncodeHex = true;

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

// JSON-string-escape a value for the small bodies we build by hand (auth +
// nutrition), matching vitalera.cpp.
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

// HMAC-SHA256(secret, data) in the wire encoding selected above (see header).
std::string sign_hmac(const std::string& secret, const std::string& data) {
    std::array<unsigned char, 32> mac = storage::hmac_sha256(secret, data);
    if (kSigEncodeHex) {
        return storage::hex_encode(mac.data(), mac.size());
    }
    return storage::base64_encode(mac.data(), mac.size());
}

//------------------------------------------------------------------------------

class Spike : public VendorBase {
public:
    explicit Spike(VendorConfig cfg) : VendorBase(make_info(), std::move(cfg)) {}

    // Fetch `domain` for `user_id` over [start_iso, end_iso] as Spike's JSON.
    // `user_id` is the application_user_id minted into the per-user JWT. The
    // domain selects the query family:
    //   Sleep              -> GET /queries/sleeps          (local-date bounds)
    //   Activity           -> GET /queries/workouts        (UTC date-time bounds)
    //   Labs               -> GET /lab_reports             (UTC date-time bounds)
    //   HeartRate/Glucose/BodyMetrics -> GET /queries/timeseries?metric=...
    // Empty bounds omit the corresponding param (the vendor's default window).
    // Nutrition is not a time-range fetch on Spike — it is the Nutrition AI
    // ingest path (analyze_nutrition_image / _label below), so it is rejected
    // here with a pointer to those operations rather than guessing an endpoint.
    std::string fetch(const std::string& user_id,
                      DataDomain domain,
                      const std::string& start_iso,
                      const std::string& end_iso) override {
        require_configured();
        const std::string uid(user_id);
        if (uid.empty()) {
            throw VendorError(info_.id + ": fetch requires a user_id (your application_user_id)");
        }

        switch (domain) {
            case DataDomain::Sleep:
                return get_json(
                    base_url() + "queries/sleeps" + date_range(start_iso, end_iso, /*as_date=*/true),
                    "fetch queries/sleeps", uid);
            case DataDomain::Activity:
                return get_json(
                    base_url() + "queries/workouts" + date_range(start_iso, end_iso, /*as_date=*/false),
                    "fetch queries/workouts", uid);
            case DataDomain::Labs:
                return get_json(
                    base_url() + "lab_reports" + date_range(start_iso, end_iso, /*as_date=*/false),
                    "fetch lab_reports", uid);
            case DataDomain::HeartRate:
            case DataDomain::Glucose:
            case DataDomain::BodyMetrics: {
                const char* metric = timeseries_metric(domain);
                std::string url = base_url() + "queries/timeseries?metric=" + url_encode(metric) +
                                  date_range(start_iso, end_iso, /*as_date=*/false, /*lead_amp=*/true);
                return get_json(url, "fetch queries/timeseries", uid);
            }
            case DataDomain::Nutrition:
                throw VendorError(info_.id + ": nutrition is not a time-range fetch on Spike — use "
                                  "analyze_nutrition_image()/analyze_nutrition_label() (Nutrition AI)");
            default:
                throw VendorError(std::string(info_.id) + ": unsupported domain '" +
                                  to_string(domain) + "' for fetch");
        }
    }

    //--------------------------------------------------------------------------
    // Defining operation — Nutrition AI (confirmed contract).
    //--------------------------------------------------------------------------

    // Food-image recognition: POST /nutrition_records/image. `image_base64` is
    // base64-encoded image bytes (the `body` field); `analysis_mode` is "precise"
    // (default) or "fast". `user_id` selects the application_user_id the record is
    // written under. Returns the structured nutrition JSON verbatim.
    std::string analyze_nutrition_image(const std::string& user_id,
                                        const std::string& image_base64,
                                        const std::string& analysis_mode = "precise") {
        return post_nutrition("nutrition_records/image", user_id, image_base64, analysis_mode,
                              "analyze nutrition_records/image");
    }

    // Nutrition-facts label recognition: POST /nutrition_records/ingredients/label.
    // Same body shape as analyze_nutrition_image (synchronous, no webhook).
    std::string analyze_nutrition_label(const std::string& user_id,
                                        const std::string& image_base64,
                                        const std::string& analysis_mode = "precise") {
        return post_nutrition("nutrition_records/ingredients/label", user_id, image_base64,
                              analysis_mode, "analyze nutrition_records/ingredients/label");
    }

    //--------------------------------------------------------------------------
    // Webhook (confirmed contract).
    //--------------------------------------------------------------------------

    // Verify and parse an inbound webhook delivery. Spike signs the raw body with
    // HMAC-SHA256 under the console shared secret and delivers the digest in the
    // X-Body-Signature header; we recompute it over `body` and constant-time
    // compare. On success returns `body` (a JSON array of event objects) verbatim.
    // `raw_headers` is the literal request header block ("Name: value" per line).
    std::string handle_webhook(const std::string& raw_headers,
                               const std::string& body) override {
        const std::string& secret = shared_secret();
        if (secret.empty()) {
            throw VendorError(info_.id + ": handle_webhook requires the shared secret — set "
                              "MIROBODY_VENDOR_SPIKE_API_KEY (or _CLIENT_SECRET)");
        }
        const std::string provided = header_value(std::string(raw_headers), kWebhookSigHeader);
        if (provided.empty()) {
            throw VendorError(std::string(info_.id) + ": webhook missing X-Body-Signature header");
        }
        const std::string expected = sign_hmac(secret, std::string(body));
        if (!constant_time_eq(provided, expected)) {
            throw VendorError(std::string(info_.id) + ": webhook signature mismatch (X-Body-Signature)");
        }
        return std::string(body);
    }

    // Disconnect the user from one provider: DELETE /providers/{slug}/integration.
    // Spike scopes the delete by the per-user JWT, so `user_id` selects the user;
    // the provider slug comes from config.client_id-adjacent context is N/A here,
    // so the caller cannot specify a slug through this signature — see note below.
    void revoke(const std::string& /*user_id*/) override {
        // The published delete endpoint is per-provider (/providers/{slug}/
        // integration); the Vendor interface carries no provider slug, so there
        // is no faithful way to target a specific integration from here. Rather
        // than guess an undocumented "delete all integrations" route, surface
        // this honestly.
        throw VendorError(info_.id + ": revoke needs a provider slug — Spike disconnects per "
                          "provider via DELETE /providers/{slug}/integration, which the Vendor "
                          "interface does not currently carry");
    }

private:
    //--------------------------------------------------------------------------
    // Operations support
    //--------------------------------------------------------------------------

    std::string post_nutrition(const char* path,
                               const std::string& user_id,
                               const std::string& image_base64,
                               const std::string& analysis_mode,
                               const std::string& op) {
        require_configured();
        const std::string uid(user_id);
        if (uid.empty()) {
            throw VendorError(info_.id + ": " + op + " requires a user_id (your application_user_id)");
        }
        if (image_base64.empty()) {
            throw VendorError(info_.id + ": " + op + " requires base64-encoded image bytes (body)");
        }
        const std::string mode = analysis_mode.empty() ? std::string("precise")
                                                        : std::string(analysis_mode);

        client::HttpRequest req;
        req.url  = base_url() + path;
        req.body = std::string("{\"body\":\"") + json_escape(std::string(image_base64)) +
                   "\",\"analysis_mode\":\"" + json_escape(mode) + "\"}";
        req.headers = auth_headers(uid);
        client::HttpResponse res = client::HttpClient().post(req);
        if (res.status < 200 || res.status >= 300) {
            throw VendorError(http_error(op, res));
        }
        return res.body;
    }

    // Build the "?from..=..&to..=.." (or "&from..=.." when `lead_amp`) suffix.
    // `as_date` switches between the local-date params (from_date/to_date) used by
    // /queries/sleeps and the UTC date-time params (from_timestamp/to_timestamp)
    // used by timeseries/workouts/lab_reports.
    std::string date_range(const std::string& start_iso,
                           const std::string& end_iso,
                           bool as_date,
                           bool lead_amp = false) const {
        const char* from_key = as_date ? "from_date" : "from_timestamp";
        const char* to_key   = as_date ? "to_date"   : "to_timestamp";
        std::string out;
        bool first = !lead_amp;
        if (!start_iso.empty()) {
            out += (first ? "?" : "&");
            out += std::string(from_key) + "=" + url_encode(std::string(start_iso));
            first = false;
        }
        if (!end_iso.empty()) {
            out += (first ? "?" : "&");
            out += std::string(to_key) + "=" + url_encode(std::string(end_iso));
        }
        return out;
    }

    // Map a DataDomain onto the timeseries `metric` slug (confirmed slugs from the
    // metrics matrix). Domains not served by a single timeseries metric never
    // reach here (handled explicitly in fetch()).
    static const char* timeseries_metric(DataDomain d) {
        switch (d) {
            case DataDomain::HeartRate:   return "heartrate";
            case DataDomain::Glucose:     return "blood_glucose";
            case DataDomain::BodyMetrics: return "weight";
            default:                      return "heartrate";
        }
    }

    //--------------------------------------------------------------------------
    // Transport + auth
    //--------------------------------------------------------------------------

    std::string get_json(const std::string& url, const std::string& op, const std::string& uid) {
        client::HttpResponse res =
            client::HttpClient().get(url, /*timeout_ms=*/30000, auth_headers(uid));
        if (res.status < 200 || res.status >= 300) {
            throw VendorError(http_error(op, res));
        }
        return res.body;
    }

    std::vector<std::string> auth_headers(const std::string& uid) {
        return { "Authorization: Bearer " + bearer(uid) };
    }

    // Resolve a per-user JWT bearer, caching it for this client's lifetime keyed
    // on the application_user_id. If config carries a pre-issued api_key bearer
    // *and no client_id*, it is used directly (api-key mode); otherwise we mint a
    // token via POST /auth/hmac using the shared secret.
    const std::string& bearer(const std::string& uid) {
        std::lock_guard<std::mutex> lk(token_mu_);
        if (config().client_id.empty() && !config().api_key.empty() &&
            config().client_secret.empty()) {
            // No application_id + no secret to sign with → treat api_key as a
            // ready-made bearer token.
            token_ = config().api_key;
            return token_;
        }
        std::string& cached = tokens_[uid];
        if (cached.empty()) {
            cached = mint_token(uid);
        }
        return cached;
    }

    std::string mint_token(const std::string& uid) {
        const std::string& secret = shared_secret();
        if (config().client_id.empty() || secret.empty()) {
            throw VendorError(info_.id + ": auth requires application_id + shared secret — set "
                              "MIROBODY_VENDOR_SPIKE_CLIENT_ID and "
                              "MIROBODY_VENDOR_SPIKE_API_KEY (or _CLIENT_SECRET)");
        }
        const std::string signature = sign_hmac(secret, uid);

        client::HttpRequest req;
        req.url  = base_url() + "auth/hmac";
        req.body = std::string("{\"") + kAuthAppIdField + "\":\"" +
                   json_escape(config().client_id) + "\",\"" +
                   kAuthAppUserIdField + "\":\"" + json_escape(uid) + "\",\"" +
                   kAuthSignatureField + "\":\"" + json_escape(signature) + "\"}";
        client::HttpResponse res = client::HttpClient().post(req);
        if (res.status < 200 || res.status >= 300) {
            throw VendorError(http_error("authenticate (auth/hmac)", res));
        }

        rapidjson::Document doc;
        if (doc.Parse(res.body.c_str()).HasParseError() || !doc.IsObject()) {
            throw VendorError(info_.id + ": auth response was not a JSON object");
        }
        for (const char* field : { "access_token", "token", "jwt" }) {
            if (doc.HasMember(field) && doc[field].IsString()) {
                return doc[field].GetString();
            }
        }
        throw VendorError(info_.id + ": no access_token field in auth response");
    }

    // The console shared secret used both to sign the auth challenge and to verify
    // webhook bodies. We accept it from api_key (primary) or client_secret.
    const std::string& shared_secret() const {
        return config().api_key.empty() ? config().client_secret : config().api_key;
    }

    //--------------------------------------------------------------------------
    // Misc
    //--------------------------------------------------------------------------

    void require_configured() const {
        if (!config().configured()) {
            throw VendorError(info_.id + ": not configured — set "
                              "MIROBODY_VENDOR_SPIKE_API_KEY (shared secret) and "
                              "MIROBODY_VENDOR_SPIKE_CLIENT_ID (application_id)");
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

    // Pull a header value out of a raw "Name: value\r\n"... block, matching on a
    // lowercased name. Returns "" when absent.
    static std::string header_value(const std::string& raw, const std::string& lname) {
        std::size_t pos = 0;
        while (pos < raw.size()) {
            std::size_t eol = raw.find('\n', pos);
            std::string line = raw.substr(pos, eol == std::string::npos ? std::string::npos
                                                                        : eol - pos);
            pos = (eol == std::string::npos) ? raw.size() : eol + 1;
            std::size_t colon = line.find(':');
            if (colon == std::string::npos) continue;
            std::string name = line.substr(0, colon);
            for (char& c : name) c = static_cast<char>(std::tolower((unsigned char)c));
            // trim trailing CR/space from name
            while (!name.empty() && (name.back() == ' ' || name.back() == '\r')) name.pop_back();
            if (name != lname) continue;
            std::string val = line.substr(colon + 1);
            std::size_t b = val.find_first_not_of(" \t");
            std::size_t e = val.find_last_not_of(" \t\r");
            if (b == std::string::npos) return std::string();
            return val.substr(b, e - b + 1);
        }
        return std::string();
    }

    // Length-aware constant-time string compare (avoids leaking via early-exit).
    static bool constant_time_eq(const std::string& a, const std::string& b) {
        if (a.size() != b.size()) return false;
        unsigned char diff = 0;
        for (std::size_t i = 0; i < a.size(); ++i) {
            diff |= static_cast<unsigned char>(a[i] ^ b[i]);
        }
        return diff == 0;
    }

    static VendorInfo make_info() {
        VendorInfo i;
        i.id                   = "spike";
        i.display_name         = "Spike API";
        i.positioning          = "Health data gateway + AI nutrition";
        i.target_customers     = "App developers, fitness apps, labs";
        i.data_source_coverage = "Activity, sleep, heart rate, nutrition, lab reports";
        i.integration_method   = "REST API, mobile SDK, webhook";
        i.compliance_summary   = "Not publicly disclosed";
        i.differentiator       = "Built-in Nutrition AI (food-image and nutrition-label recognition)";
        i.docs_url             = "https://docs.spikeapi.com/overview";
        i.region               = Region::Global;
        i.open_source          = false;
        i.domains              = {DataDomain::Activity, DataDomain::Sleep, DataDomain::HeartRate,
                                  DataDomain::Nutrition, DataDomain::Labs};
        i.integrations         = {Integration::Rest, Integration::Sdk, Integration::Webhook};
        return i;
    }

    std::mutex token_mu_;
    std::string token_;                                   // api-key-mode bearer
    std::unordered_map<std::string, std::string> tokens_; // per-user minted JWTs
};

}

std::unique_ptr<Vendor> make_spike(const VendorConfig& cfg) {
    return std::unique_ptr<Vendor>(new Spike(cfg));
}

}}

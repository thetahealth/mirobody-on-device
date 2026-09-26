#include "health/ehr_connect.hpp"
#include <optional>

#include "cache/cache.hpp"
#include "client/http_client.hpp"
#include "health/vendor/ehr/directory.hpp"
#include "health/vendor/registry.hpp"
#include "health/vendor/vendor.hpp"
#include "oauth/pkce.hpp"
#include "platform/clock.hpp"
#include "server/auth.hpp"

#include <rapidjson/document.h>
#include <rapidjson/stringbuffer.h>
#include <rapidjson/writer.h>

#include <cctype>
#include <chrono>
#include <exception>
#include <memory>
#include <string>
#include <vector>

namespace mirobody { namespace health {

namespace {

// Read a string member from a JSON value; "" when absent or not a string.
std::string json_str(const rapidjson::Value& v, const char* key) {
    if (!v.IsObject() || !v.HasMember(key)) return std::string();
    const rapidjson::Value& m = v[key];
    return m.IsString() ? std::string(m.GetString(), m.GetStringLength()) : std::string();
}

// Percent-encode a query/form component (RFC 3986 unreserved pass through).
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

// Serialize a JSON value (a Bundle entry's resource) back to a string.
std::string serialize(const rapidjson::Value& v) {
    rapidjson::StringBuffer buf;
    rapidjson::Writer<rapidjson::StringBuffer> w(buf);
    v.Accept(w);
    return std::string(buf.GetString(), buf.GetSize());
}

// SMART discovery: GET {base}/.well-known/smart-configuration -> the authorize /
// token endpoints. Returns false (and sets err) on failure.
bool discover(const std::string& base, std::string& authorize_ep,
              std::string& token_ep, std::string& err) {
    std::string url = base;
    if (!url.empty() && url.back() != '/') url.push_back('/');
    url += ".well-known/smart-configuration";

    client::HttpResponse res =
        client::HttpClient().get(url, /*timeout_ms=*/15000, {"Accept: application/json"});
    if (res.status < 200 || res.status >= 300) {
        err = "SMART discovery failed (HTTP " + std::to_string(res.status) + ")";
        return false;
    }
    rapidjson::Document d;
    d.Parse(res.body.c_str(), res.body.size());
    if (d.HasParseError() || !d.IsObject()) {
        err = "SMART discovery returned invalid JSON";
        return false;
    }
    authorize_ep = json_str(d, "authorization_endpoint");
    token_ep     = json_str(d, "token_endpoint");
    if (authorize_ep.empty() || token_ep.empty()) {
        err = "SMART discovery missing authorization_endpoint / token_endpoint";
        return false;
    }
    return true;
}

const char* kStatePrefix = "ehr:state:";
const char* kTokenPrefix = "ehr:token:";

}  // namespace

//------------------------------------------------------------------------------

EhrConnectService::EhrConnectService(server::Router& router, const Config& cfg,
                                     database::Database& db, cache::Cache& cache,
                                     const jwt::Jwt& jwt)
    : cfg_(cfg), fhir_store_(db), cache_(cache), jwt_(jwt) {
    register_routes(router);
}

void EhrConnectService::register_routes(server::Router& router) {
    router.get("/health/ehr/providers",
               server::require_auth(jwt_, [this](const server::Request& q, server::Response& s){ on_providers(q, s); }));
    router.post("/health/ehr/authorize",
                server::require_auth(jwt_, [this](const server::Request& q, server::Response& s){ on_authorize(q, s); }));
    // Callback carries no bearer (it's the EHR redirecting the browser); the user
    // is recovered from the single-use `state` record instead.
    router.get("/health/ehr/callback",
               [this](const server::Request& q, server::Response& s){ on_callback(q, s); });
    router.post("/health/ehr/sync",
                server::require_auth(jwt_, [this](const server::Request& q, server::Response& s){ on_sync(q, s); }));
}

//------------------------------------------------------------------------------

// GET /health/ehr/providers?q=&source= — list EHR tenants (org name + FHIR base
// URL) from the public Service Base URL directories, filtered by `q` (substring,
// case-insensitive) and capped. `source` is "oracle_health" (default — verified,
// compact) or "lantern" (national, large). Live fetch; cache later if needed.
void EhrConnectService::on_providers(const server::Request& req, server::Response& res) {
    std::string q = req.query_get("q");
    for (char& c : q) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    const std::string source = req.query_get("source");

    vendor::LanternDirectory lantern;
    vendor::OracleHealthDirectory oracle;
    std::vector<vendor::EhrDirectory*> sources;
    if (source == "lantern") {
        sources.push_back(&lantern);
    } else {
        sources.push_back(&oracle);   // default: the verified, smaller list
    }

    std::vector<std::string> errs;
    std::vector<vendor::EhrEndpoint> all = vendor::fetch_all(sources, &errs);
    if (all.empty() && !errs.empty()) {
        res.error(-1, errs.front());
        return;
    }

    rapidjson::StringBuffer buf;
    rapidjson::Writer<rapidjson::StringBuffer> w(buf);
    w.StartObject();
    w.Key("providers");
    w.StartArray();
    int emitted = 0;
    for (const vendor::EhrEndpoint& ep : all) {
        if (emitted >= 50) break;   // cap; refine with paging/search later
        if (!q.empty()) {
            std::string name = ep.name;
            for (char& c : name) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            if (name.find(q) == std::string::npos) continue;
        }
        w.StartObject();
        w.Key("name");          w.String(ep.name.data(), static_cast<rapidjson::SizeType>(ep.name.size()));
        w.Key("fhir_base_url"); w.String(ep.fhir_base_url.data(), static_cast<rapidjson::SizeType>(ep.fhir_base_url.size()));
        w.EndObject();
        ++emitted;
    }
    w.EndArray();
    w.EndObject();
    res.ok(std::string(buf.GetString(), buf.GetSize()));
}

//------------------------------------------------------------------------------

// POST /health/ehr/authorize {"fhir_base_url": "..."} — discover the tenant's
// SMART endpoints, mint a PKCE verifier + state, stash them in the cache, and
// return the authorize URL for the browser to redirect to.
void EhrConnectService::on_authorize(const server::Request& req, server::Response& res) {
    if (cfg_.smart_fhir.client_id.empty() || cfg_.smart_fhir.redirect_uri.empty()) {
        res.error(-1, "SMART client not configured (set SMART_FHIR_CLIENT_ID and SMART_FHIR_REDIRECT_URI).");
        return;
    }

    rapidjson::Document body;
    body.Parse(req.body.c_str(), req.body.size());
    const std::string base = (body.HasParseError() || !body.IsObject())
                                 ? std::string() : json_str(body, "fhir_base_url");
    if (base.empty()) {
        res.error(-2, "Missing fhir_base_url.");
        return;
    }

    std::string authorize_ep, token_ep, derr;
    if (!discover(base, authorize_ep, token_ep, derr)) {
        res.error(-3, derr);
        return;
    }

    const std::string verifier  = oauth::random_token(32);
    const std::string challenge = oauth::base64url_encode(oauth::sha256(verifier));
    const std::string state     = oauth::random_token(24);

    // Stash the pending authorization, single-use, short TTL.
    {
        rapidjson::StringBuffer sb;
        rapidjson::Writer<rapidjson::StringBuffer> w(sb);
        w.StartObject();
        w.Key("user_id");        w.Int64(req.user_id);
        w.Key("verifier");       w.String(verifier.data(), static_cast<rapidjson::SizeType>(verifier.size()));
        w.Key("fhir_base_url");  w.String(base.data(), static_cast<rapidjson::SizeType>(base.size()));
        w.Key("token_endpoint"); w.String(token_ep.data(), static_cast<rapidjson::SizeType>(token_ep.size()));
        w.EndObject();
        cache_.set(kStatePrefix + state, std::string(sb.GetString(), sb.GetSize()),
                   std::chrono::seconds(cfg_.smart_fhir.state_ttl));
    }

    std::string url = authorize_ep;
    url += (authorize_ep.find('?') == std::string::npos) ? '?' : '&';
    url += "response_type=code";
    url += "&client_id="     + url_encode(cfg_.smart_fhir.client_id);
    url += "&redirect_uri="  + url_encode(cfg_.smart_fhir.redirect_uri);
    url += "&scope="         + url_encode(cfg_.smart_fhir.scope);
    url += "&state="         + url_encode(state);
    url += "&aud="           + url_encode(base);
    url += "&code_challenge=" + url_encode(challenge);
    url += "&code_challenge_method=S256";

    rapidjson::StringBuffer ob;
    rapidjson::Writer<rapidjson::StringBuffer> ow(ob);
    ow.StartObject();
    ow.Key("authorize_url"); ow.String(url.data(), static_cast<rapidjson::SizeType>(url.size()));
    ow.EndObject();
    res.ok(std::string(ob.GetString(), ob.GetSize()));
}

//------------------------------------------------------------------------------

// GET /health/ehr/callback?code=&state= — the EHR's browser redirect back. Look
// up the state, exchange the code at the tenant's token endpoint, cache the
// access token per user, and 302 the browser back to the app.
void EhrConnectService::on_callback(const server::Request& req, server::Response& res) {
    const std::string app_root = cfg_.uri_prefix.empty() ? std::string("/") : cfg_.uri_prefix + "/";
    auto redirect = [&](const char* status) {
        res.status(302);
        res.header("Location", app_root + "?ehr=" + status);
        res.body("");
    };

    const std::string code  = req.query_get("code");
    const std::string state = req.query_get("state");
    if (code.empty() || state.empty()) { redirect("error"); return; }

    std::optional<std::string> rec = cache_.get(kStatePrefix + state);
    if (!rec.has_value()) { redirect("expired"); return; }
    cache_.del(kStatePrefix + state);   // single-use

    rapidjson::Document sd;
    sd.Parse(rec->c_str(), rec->size());
    if (sd.HasParseError() || !sd.IsObject()) { redirect("error"); return; }
    const std::int64_t user_id = sd.HasMember("user_id") && sd["user_id"].IsInt64()
                                     ? sd["user_id"].GetInt64() : 0;
    const std::string verifier = json_str(sd, "verifier");
    const std::string base     = json_str(sd, "fhir_base_url");
    const std::string token_ep = json_str(sd, "token_endpoint");
    if (user_id == 0 || verifier.empty() || base.empty() || token_ep.empty()) {
        redirect("error");
        return;
    }

    std::string form = "grant_type=authorization_code";
    form += "&code="          + url_encode(code);
    form += "&redirect_uri="  + url_encode(cfg_.smart_fhir.redirect_uri);
    form += "&client_id="     + url_encode(cfg_.smart_fhir.client_id);
    form += "&code_verifier=" + url_encode(verifier);
    if (!cfg_.smart_fhir.client_secret.empty()) {
        form += "&client_secret=" + url_encode(cfg_.smart_fhir.client_secret);
    }

    client::HttpRequest tx;
    tx.url          = token_ep;
    tx.body         = form;
    tx.content_type = "application/x-www-form-urlencoded";
    tx.headers      = {"Accept: application/json"};
    client::HttpResponse tr = client::HttpClient().post(tx);
    if (tr.status < 200 || tr.status >= 300) { redirect("token_error"); return; }

    rapidjson::Document td;
    td.Parse(tr.body.c_str(), tr.body.size());
    if (td.HasParseError() || !td.IsObject()) { redirect("token_error"); return; }
    const std::string access  = json_str(td, "access_token");
    const std::string patient = json_str(td, "patient");
    if (access.empty()) { redirect("token_error"); return; }
    std::int64_t expires = 3600;
    if (td.HasMember("expires_in") && td["expires_in"].IsInt64()) expires = td["expires_in"].GetInt64();
    else if (td.HasMember("expires_in") && td["expires_in"].IsInt()) expires = td["expires_in"].GetInt();

    rapidjson::StringBuffer sb;
    rapidjson::Writer<rapidjson::StringBuffer> w(sb);
    w.StartObject();
    w.Key("access_token");  w.String(access.data(), static_cast<rapidjson::SizeType>(access.size()));
    w.Key("fhir_base_url"); w.String(base.data(), static_cast<rapidjson::SizeType>(base.size()));
    w.Key("patient");       w.String(patient.data(), static_cast<rapidjson::SizeType>(patient.size()));
    w.EndObject();
    cache_.set(kTokenPrefix + std::to_string(user_id),
               std::string(sb.GetString(), sb.GetSize()),
               std::chrono::seconds(expires));

    redirect("connected");
}

//------------------------------------------------------------------------------

// POST /health/ehr/sync — using the cached access token, fetch the user's
// Observations from the connected EHR via the `ehr` vendor client and persist
// each one through the FHIR store. Returns posted / failed counts.
void EhrConnectService::on_sync(const server::Request& req, server::Response& res) {
    std::optional<std::string> rec = cache_.get(kTokenPrefix + std::to_string(req.user_id));
    if (!rec.has_value()) {
        res.error(-1, "No connected EHR (connect one first).");
        return;
    }
    rapidjson::Document td;
    td.Parse(rec->c_str(), rec->size());
    const std::string access  = json_str(td, "access_token");
    const std::string base    = json_str(td, "fhir_base_url");
    const std::string patient = json_str(td, "patient");
    if (access.empty() || base.empty()) {
        res.error(-2, "Stored EHR connection is incomplete; reconnect.");
        return;
    }

    vendor::VendorConfig vc;
    vc.api_key  = access;
    vc.base_url = base;
    std::unique_ptr<vendor::Vendor> client = vendor::open_vendor("ehr", vc);

    const vendor::DataDomain domains[] = {
        vendor::DataDomain::HeartRate, vendor::DataDomain::BodyMetrics,
        vendor::DataDomain::Labs,      vendor::DataDomain::Clinical,
    };
    const std::int64_t now = platform::now_unix_ms();
    int posted = 0, failed = 0, counter = 0;

    for (vendor::DataDomain domain : domains) {
        std::string bundle;
        try {
            bundle = client->fetch(patient, domain, std::string(), std::string());
        } catch (const std::exception&) {
            ++failed;
            continue;
        }
        rapidjson::Document bd;
        bd.Parse(bundle.c_str(), bundle.size());
        if (bd.HasParseError() || !bd.IsObject()) { ++failed; continue; }
        auto entry = bd.FindMember("entry");
        if (entry == bd.MemberEnd() || !entry->value.IsArray()) continue;

        for (const rapidjson::Value& e : entry->value.GetArray()) {
            if (!e.IsObject()) continue;
            auto r = e.FindMember("resource");
            if (r == e.MemberEnd() || !r->value.IsObject()) continue;
            if (json_str(r->value, "resourceType") != "Observation") continue;

            fhir::StoredResource sr;
            sr.type       = "Observation";
            sr.id         = "ehr-" + std::to_string(now) + "-" + std::to_string(counter++);
            sr.version_id = 1;
            sr.updated_at = now;
            sr.deleted    = false;
            sr.content    = serialize(r->value);
            try {
                fhir_store_.upsert(req.user_id, sr);
                ++posted;
            } catch (const std::exception&) {
                ++failed;
            }
        }
    }

    rapidjson::StringBuffer sb;
    rapidjson::Writer<rapidjson::StringBuffer> w(sb);
    w.StartObject();
    w.Key("posted"); w.Int(posted);
    w.Key("failed"); w.Int(failed);
    w.EndObject();
    res.ok(std::string(sb.GetString(), sb.GetSize()));
}

}}  // namespace mirobody::health

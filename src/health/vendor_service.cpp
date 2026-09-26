#include "health/vendor_service.hpp"

#include "server/auth.hpp"
#include "health/vendor/registry.hpp"
#include "health/vendor_fhir.hpp"
#include "fhir/store.hpp"
#include "cache/cache.hpp"
#include "client/http_client.hpp"
#include "oauth/pkce.hpp"      // random_token
#include "storage/sign.hpp"    // base64_encode
#include "platform/clock.hpp"
#include "platform/log.hpp"

#include <rapidjson/document.h>
#include <rapidjson/stringbuffer.h>
#include <rapidjson/writer.h>

#include <cctype>
#include <chrono>
#include <cstdint>
#include <exception>
#include <mutex>
#include <string>
#include <vector>

namespace mirobody { namespace health {

namespace {

// Read a string member from a JSON object; "" when absent or not a string.
std::string json_str(const rapidjson::Document& doc, const char* key) {
    if (!doc.IsObject() || !doc.HasMember(key)) return std::string();
    const rapidjson::Value& v = doc[key];
    if (v.IsString()) return std::string(v.GetString(), v.GetStringLength());
    return std::string();
}

// True when `id` names a registered vendor (src/health/vendor/registry.hpp).
bool known_vendor(const std::string& id) {
    if (id.empty()) return false;
    const std::vector<std::string> ids = vendor::vendor_ids();
    for (std::size_t i = 0; i < ids.size(); ++i) {
        if (ids[i] == id) return true;
    }
    return false;
}

// The {id} path parameter, or "" when absent.
std::string vendor_id_of(const server::Request& req) {
    std::unordered_map<std::string, std::string>::const_iterator it =
        req.path_params.find("id");
    return it == req.path_params.end() ? std::string() : it->second;
}

// Cache key for a pending OAuth connect (state -> {user_id, vendor_id}), single-use.
const char* kOAuthStatePrefix = "vendoroauth:state:";

}  // namespace

//------------------------------------------------------------------------------

VendorService::VendorService(server::Router& router, const Config& cfg,
                             database::Database& db, cache::Cache& cache, const jwt::Jwt& jwt)
    : cfg_(cfg), db_(db), cache_(cache), store_(db), jwt_(jwt) {
    register_routes(router);
    // Migrate any tokens still under an old encryption key to the current one. No-op
    // unless a rotation is in progress (>=2 keys). Runs synchronously here — before
    // the server serves requests — so it never races the single-threaded DB access;
    // best-effort, since a failure just leaves tokens under their (still-listed) old key.
    try {
        reencrypt_vendor_tokens(store_, cfg_);
    } catch (const std::exception& e) {
        platform::log_warn("vendor token re-encrypt failed: %s", e.what());
    }
}

void VendorService::register_routes(server::Router& router) {
    router.get("/vendors",
               server::require_auth(jwt_, [this](const server::Request& q, server::Response& s){ on_list(q, s); }));
    // Icons are non-sensitive + identical for everyone -> public and cacheable.
    router.get("/vendors/icons",
               [this](const server::Request& q, server::Response& s){ on_icons(q, s); });
    router.get("/vendors/{id}/authorize",
               server::require_auth(jwt_, [this](const server::Request& q, server::Response& s){ on_authorize(q, s); }));
    // Callback carries no bearer (the vendor redirects the browser); the user is
    // recovered from the single-use `state` record instead.
    router.get("/vendors/callback",
               [this](const server::Request& q, server::Response& s){ on_callback(q, s); });
    router.post("/vendors/{id}/bind",
                server::require_auth(jwt_, [this](const server::Request& q, server::Response& s){ on_bind(q, s); }));
    router.post("/vendors/{id}/bind/verify",
                server::require_auth(jwt_, [this](const server::Request& q, server::Response& s){ on_bind_verify(q, s); }));
    router.get("/vendors/{id}/data",
               server::require_auth(jwt_, [this](const server::Request& q, server::Response& s){ on_fetch(q, s); }));
    router.post("/vendors/{id}/sync",
                server::require_auth(jwt_, [this](const server::Request& q, server::Response& s){ on_sync(q, s); }));
    router.post("/vendors/{id}/unlink",
                server::require_auth(jwt_, [this](const server::Request& q, server::Response& s){ on_unlink(q, s); }));
}

//------------------------------------------------------------------------------

// GET /vendors -- list the user's connected vendors (no secrets). Each entry:
// {id, verified, has_token, updated_at}. `has_token` means a stored OAuth token is
// present (vs relying on a configured credential).
//
// Success: {"code":0,"msg":"ok","data":[{"id":..,"verified":..,"has_token":..,"updated_at":..}, ...]}
void VendorService::on_list(const server::Request& req, server::Response& res) {
    std::vector<VendorLinkStore::LinkSummary> links;
    try {
        links = store_.list(req.user_id);
    } catch (const std::exception& e) {
        res.error(-1, e.what());
        return;
    }
    rapidjson::StringBuffer sb;
    rapidjson::Writer<rapidjson::StringBuffer> w(sb);
    w.StartArray();
    for (std::size_t i = 0; i < links.size(); ++i) {
        w.StartObject();
        w.Key("id");         w.String(links[i].vendor_id.data(),
                                      static_cast<rapidjson::SizeType>(links[i].vendor_id.size()));
        w.Key("verified");   w.Bool(links[i].verified);
        w.Key("has_token");  w.Bool(links[i].has_token);
        w.Key("updated_at"); w.Int64(links[i].updated_at);
        w.EndObject();
    }
    w.EndArray();
    res.ok(std::string(sb.GetString(), sb.GetSize()));
}

// GET /vendors/icons -- always "{}": the clients fall back to monograms. The
// icons used to be fetched from each vendor's website at startup, which on a
// phone means contacting third parties before the user has done anything.
void VendorService::on_icons(const server::Request& /*req*/, server::Response& res) {
    res.ok("{}");
}

// GET /vendors/{id}/authorize -- build the vendor's OAuth authorize URL, stash a
// single-use state -> {user_id, vendor_id}, and return {authorize_url}. The browser
// redirects there; the vendor sends it back to GET /vendors/callback.
void VendorService::on_authorize(const server::Request& req, server::Response& res) {
    const std::string vendor_id = vendor_id_of(req);
    if (!known_vendor(vendor_id)) {
        res.error(-1, "Unknown vendor.");
        return;
    }
    if (cfg_.vendor_redirect_uri.empty()) {
        res.error(-2, "Vendor connect is not configured (set VENDOR_REDIRECT_URI).");
        return;
    }
    vendor::VendorConfig vc = vendor_config(cfg_, vendor_id);
    if (vc.client_id.empty()) {
        res.error(-3, "Vendor OAuth client not configured (set the vendor's <ID>_CLIENT_ID).");
        return;
    }

    std::string authorize_url;
    const std::string state = oauth::random_token(24);
    try {
        std::unique_ptr<vendor::Vendor> v = vendor::open_vendor(vendor_id, vc);
        authorize_url = v->authorize_url(cfg_.vendor_redirect_uri, state, std::string(), std::string());
    } catch (const std::exception& e) {
        res.error(-4, e.what());
        return;
    }

    {
        rapidjson::StringBuffer sb;
        rapidjson::Writer<rapidjson::StringBuffer> w(sb);
        w.StartObject();
        w.Key("user_id");   w.Int64(req.user_id);
        w.Key("vendor_id"); w.String(vendor_id.data(), static_cast<rapidjson::SizeType>(vendor_id.size()));
        w.EndObject();
        cache_.set(kOAuthStatePrefix + state, std::string(sb.GetString(), sb.GetSize()),
                   std::chrono::seconds(600));
    }

    rapidjson::StringBuffer ob;
    rapidjson::Writer<rapidjson::StringBuffer> ow(ob);
    ow.StartObject();
    ow.Key("authorize_url");
    ow.String(authorize_url.data(), static_cast<rapidjson::SizeType>(authorize_url.size()));
    ow.EndObject();
    res.ok(std::string(ob.GetString(), ob.GetSize()));
}

// GET /vendors/callback?code=&state= -- the vendor's browser redirect back. Recover
// the pending connect from `state`, exchange the code + store tokens + mark verified
// (health::oauth_connect), and 302 the browser back to the app with ?vendor=<status>.
void VendorService::on_callback(const server::Request& req, server::Response& res) {
    const std::string app_root = cfg_.uri_prefix.empty() ? std::string("/") : cfg_.uri_prefix + "/";
    auto redirect = [&](const char* status) {
        res.status(302);
        res.header("Location", app_root + "?vendor=" + status);
        res.body("");
    };

    const std::string code  = req.query_get("code");
    const std::string state = req.query_get("state");
    if (code.empty() || state.empty()) { redirect("error"); return; }

    mirobody::optional<std::string> rec = cache_.get(kOAuthStatePrefix + state);
    if (!rec.has_value()) { redirect("expired"); return; }
    cache_.del(kOAuthStatePrefix + state);   // single-use

    rapidjson::Document sd;
    sd.Parse(rec->c_str(), rec->size());
    if (sd.HasParseError() || !sd.IsObject()) { redirect("error"); return; }
    const std::int64_t user_id = (sd.HasMember("user_id") && sd["user_id"].IsInt64())
                                     ? sd["user_id"].GetInt64() : 0;
    const std::string vendor_id = json_str(sd, "vendor_id");
    if (user_id == 0 || vendor_id.empty()) { redirect("error"); return; }

    std::string err;
    if (oauth_connect(store_, cfg_, user_id, vendor_id, code, cfg_.vendor_redirect_uri, &err)) {
        redirect("connected");
    } else {
        redirect("error");
    }
}

// POST /vendors/{id}/bind -- record a PENDING link to the vendor account id.
// require_auth guarantees req.user_id > 0. The account is not usable until
// /bind/verify proves ownership.
//
// Body:    {"external_user_id": "<vendor patient/user id>"}
// Success: {"code": 0, "msg": "ok", "data": {"status": "pending"}}
void VendorService::on_bind(const server::Request& req, server::Response& res) {
    const std::string vendor_id = vendor_id_of(req);
    if (!known_vendor(vendor_id)) {
        res.error(-1, "Unknown vendor.");
        return;
    }

    rapidjson::Document doc;
    doc.Parse(req.body.c_str(), req.body.size());
    if (doc.HasParseError() || !doc.IsObject()) {
        res.error(-2, "Invalid JSON body.");
        return;
    }
    const std::string external_id = json_str(doc, "external_user_id");
    if (external_id.empty()) {
        res.error(-3, "Missing external_user_id.");
        return;
    }

    try {
        store_.upsert_pending(req.user_id, vendor_id, external_id);
    } catch (const std::exception& e) {
        // The UNIQUE(vendor_id, external_user_id) backstop lands here when another
        // user already holds this vendor account.
        res.error(-4, std::string("Bind failed: ") + e.what());
        return;
    }

    res.ok("{\"status\":\"pending\"}");
}

// POST /vendors/{id}/bind/verify -- prove ownership of the pending link, then
// mark it verified so /data may use it. The proof mechanism is vendor-specific
// (health::verify_consent).
//
// Body:    {"consent_token": "<vendor consent/claim token>"}
// Success: {"code": 0, "msg": "ok", "data": {"status": "verified"}}
void VendorService::on_bind_verify(const server::Request& req, server::Response& res) {
    const std::string vendor_id = vendor_id_of(req);
    if (!known_vendor(vendor_id)) {
        res.error(-1, "Unknown vendor.");
        return;
    }

    VendorLinkStore::Link link;
    bool found = false;
    try {
        found = store_.get(req.user_id, vendor_id, &link);
    } catch (const std::exception& e) {
        res.error(-2, e.what());
        return;
    }
    if (!found) {
        res.error(-3, "No pending link to verify (bind first).");
        return;
    }

    rapidjson::Document doc;
    doc.Parse(req.body.c_str(), req.body.size());
    // OAuth flow: {code, redirect_uri} → server exchanges the code and stores the
    // resulting tokens (encrypted). Empty code → fall back to a configured-credential
    // probe (self-hosted / single-user). See health::verify_consent.
    const std::string code         = json_str(doc, "code");
    const std::string redirect_uri = json_str(doc, "redirect_uri");

    std::string verr;
    if (!verify_consent(store_, cfg_, req.user_id, vendor_id, link.external_user_id,
                        code, redirect_uri, &verr)) {
        res.error(-4, verr.empty() ? std::string("Verification failed.") : verr);
        return;
    }

    try {
        store_.mark_verified(req.user_id, vendor_id);
    } catch (const std::exception& e) {
        res.error(-5, e.what());
        return;
    }

    res.ok("{\"status\":\"verified\"}");
}

// GET /vendors/{id}/data?domain=&start=&end= -- fetch the user's data for the
// linked (and verified) vendor account. The vendor's JSON is spliced verbatim as
// the envelope `data`.
//
// Success: {"code": 0, "msg": "ok", "data": <vendor JSON>}
void VendorService::on_fetch(const server::Request& req, server::Response& res) {
    const std::string vendor_id = vendor_id_of(req);
    if (!known_vendor(vendor_id)) {
        res.error(-1, "Unknown vendor.");
        return;
    }

    const std::string domain_s = req.query_get("domain");
    vendor::DataDomain domain;
    if (domain_s.empty() || !vendor::parse_domain(domain_s, domain)) {
        res.error(-2, "Missing or invalid 'domain' query parameter.");
        return;
    }
    const std::string start = req.query_get("start");
    const std::string end   = req.query_get("end");

    std::string err;
    const std::string json =
        fetch_for_user(store_, cfg_, req.user_id, vendor_id, domain, start, end, &err);
    if (!err.empty()) {
        res.error(-3, err);
        return;
    }
    res.ok(json);
}

// POST /vendors/{id}/sync?domain=&start=&end= -- fetch the user's data for the
// verified vendor link, map the vendor-native JSON to FHIR R4 Observations
// (health::vendor_json_to_observations), and persist each through the FHIR store —
// the same write path the on-device apps and EHR connect flow use. With no ?domain,
// every domain the vendor advertises (Vendor::info().domains) is synced; unmappable
// vendors/domains simply contribute zero Observations. Mirrors EhrConnect::on_sync,
// with the mapping step these non-FHIR vendors need.
//
// Success: {"code": 0, "msg": "ok", "data": {"posted": N, "failed": M}}
void VendorService::on_sync(const server::Request& req, server::Response& res) {
    const std::string vendor_id = vendor_id_of(req);
    if (!known_vendor(vendor_id)) {
        res.error(-1, "Unknown vendor.");
        return;
    }

    // Domains to sync: the one requested, or all the vendor advertises.
    std::vector<vendor::DataDomain> domains;
    const std::string domain_s = req.query_get("domain");
    if (!domain_s.empty()) {
        vendor::DataDomain d;
        if (!vendor::parse_domain(domain_s, d)) {
            res.error(-2, "Invalid 'domain' query parameter.");
            return;
        }
        domains.push_back(d);
    } else {
        // info() needs no credentials or network — safe to build a bare client.
        try {
            domains = vendor::open_vendor(vendor_id, vendor::VendorConfig())->info().domains;
        } catch (const std::exception& e) {
            res.error(-2, e.what());
            return;
        }
    }

    const std::string start   = req.query_get("start");
    const std::string end     = req.query_get("end");
    const std::string subject = "Patient/" + std::to_string(req.user_id);
    const std::int64_t now    = platform::now_unix_ms();

    fhir::FhirStore fhir_store(db_);
    int posted = 0, failed = 0, counter = 0;

    for (std::size_t i = 0; i < domains.size(); ++i) {
        std::string err;
        const std::string json =
            fetch_for_user(store_, cfg_, req.user_id, vendor_id, domains[i], start, end, &err);
        if (!err.empty()) {
            // No verified link, unverified, or the vendor does not broker this domain
            // (its fetch throws): count as a failure for this domain and move on.
            ++failed;
            continue;
        }
        const std::vector<std::string> obs =
            vendor_json_to_observations(vendor_id, domains[i], json, subject);
        for (std::size_t j = 0; j < obs.size(); ++j) {
            fhir::StoredResource sr;
            sr.type       = "Observation";
            sr.id         = vendor_id + "-" + std::to_string(now) + "-" + std::to_string(counter++);
            sr.version_id = 1;
            sr.updated_at = now;
            sr.deleted    = false;
            sr.content    = obs[j];
            try {
                fhir_store.upsert(req.user_id, sr);
                ++posted;
            } catch (const std::exception&) {
                ++failed;
            }
        }
    }

    res.ok("{\"posted\":" + std::to_string(posted) +
           ",\"failed\":" + std::to_string(failed) + "}");
}

// POST /vendors/{id}/unlink — disconnect the user from the vendor: best-effort
// revoke the grant at the vendor, then delete the link row (stored tokens included).
// Idempotent: returns {"status":"unlinked"} whether or not a link existed.
void VendorService::on_unlink(const server::Request& req, server::Response& res) {
    const std::string vendor_id = vendor_id_of(req);
    if (!known_vendor(vendor_id)) {
        res.error(-1, "Unknown vendor.");
        return;
    }
    try {
        unlink_for_user(store_, cfg_, req.user_id, vendor_id);
    } catch (const std::exception& e) {
        res.error(-2, e.what());
        return;
    }
    res.ok("{\"status\":\"unlinked\"}");
}

}}  // namespace mirobody::health

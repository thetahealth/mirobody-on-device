#include "health/vendor_service.hpp"

#include "server/auth.hpp"
#include "health/vendor/registry.hpp"

#include <rapidjson/document.h>

#include <cctype>
#include <exception>
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

}  // namespace

//------------------------------------------------------------------------------

VendorService::VendorService(server::Router& router, const Config& cfg,
                             database::Database& db, const jwt::Jwt& jwt)
    : cfg_(cfg), store_(db), jwt_(jwt) {
    register_routes(router);
}

void VendorService::register_routes(server::Router& router) {
    router.post("/vendors/{id}/bind",
                server::require_auth(jwt_, [this](const server::Request& q, server::Response& s){ on_bind(q, s); }));
    router.post("/vendors/{id}/bind/verify",
                server::require_auth(jwt_, [this](const server::Request& q, server::Response& s){ on_bind_verify(q, s); }));
    router.get("/vendors/{id}/data",
               server::require_auth(jwt_, [this](const server::Request& q, server::Response& s){ on_fetch(q, s); }));
}

//------------------------------------------------------------------------------

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
    const std::string consent_token =
        (doc.HasParseError() || !doc.IsObject()) ? std::string() : json_str(doc, "consent_token");

    std::string verr;
    if (!verify_consent(cfg_, vendor_id, link.external_user_id, consent_token, &verr)) {
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

}}  // namespace mirobody::health

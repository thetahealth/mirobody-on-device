#include "fhir/rest.hpp"

#include "fhir/fhir.hpp"
#include "fhir/resource.hpp"
#include "platform/clock.hpp"   // now_unix_ms
#include "server/auth.hpp"

#include <rapidjson/document.h>
#include <rapidjson/stringbuffer.h>
#include <rapidjson/writer.h>

#include <ctime>
#include <cstdio>
#include <random>
#include <string>

namespace mirobody { namespace fhir {

namespace {

using rapidjson::Document;
using rapidjson::Value;

std::string serialize(const Value& v) {
    rapidjson::StringBuffer buf;
    rapidjson::Writer<rapidjson::StringBuffer> w(buf);
    v.Accept(w);
    return std::string(buf.GetString(), buf.GetSize());
}

// ISO-8601 UTC instant for a unix-ms timestamp, e.g. "2026-06-08T11:18:06Z".
// The fhir_resources lifecycle columns are unix ms (see fhir/store.hpp); this
// renders one back to the FHIR instant used for meta.lastUpdated and the
// Last-Modified header (seconds resolution, the FHIR `instant` granularity).
// Single service thread, so gmtime needs no extra locking beyond the reentrant form.
std::string iso8601_from_unix_ms(std::int64_t ms) {
    std::time_t t = static_cast<std::time_t>(ms / 1000);
    std::tm tmv;
#ifdef _WIN32
    gmtime_s(&tmv, &t);
#else
    gmtime_r(&t, &tmv);
#endif
    char buf[32];
    std::strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%SZ", &tmv);
    return std::string(buf);
}

// RFC-4122 v4 UUID for server-assigned resource ids. Seeded once; std::random
// is fine here (ordinary server code, not a workflow script).
std::string gen_uuid() {
    static std::mt19937_64 rng((std::random_device()()));
    std::uniform_int_distribution<std::uint64_t> dist;
    std::uint64_t a = dist(rng), b = dist(rng);
    a = (a & 0xFFFFFFFFFFFF0FFFULL) | 0x0000000000004000ULL;  // version 4
    b = (b & 0x3FFFFFFFFFFFFFFFULL) | 0x8000000000000000ULL;  // variant 1
    char s[37];
    std::snprintf(s, sizeof(s), "%08x-%04x-%04x-%04x-%012llx",
                  static_cast<unsigned>(a >> 32),
                  static_cast<unsigned>((a >> 16) & 0xFFFF),
                  static_cast<unsigned>(a & 0xFFFF),
                  static_cast<unsigned>(b >> 48),
                  static_cast<unsigned long long>(b & 0xFFFFFFFFFFFFULL));
    return std::string(s);
}

// Build an OperationOutcome JSON document.
std::string operation_outcome(const std::vector<ValidationIssue>& issues) {
    Document d;
    d.SetObject();
    Document::AllocatorType& a = d.GetAllocator();
    d.AddMember("resourceType", "OperationOutcome", a);
    Value arr(rapidjson::kArrayType);
    for (size_t i = 0; i < issues.size(); ++i) {
        Value iss(rapidjson::kObjectType);
        iss.AddMember("severity", Value(issues[i].severity.c_str(), a), a);
        iss.AddMember("code", Value(issues[i].code.c_str(), a), a);
        Value det(rapidjson::kObjectType);
        det.AddMember("text", Value(issues[i].diagnostics.c_str(), a), a);
        iss.AddMember("details", det, a);
        iss.AddMember("diagnostics", Value(issues[i].diagnostics.c_str(), a), a);
        arr.PushBack(iss, a);
    }
    d.AddMember("issue", arr, a);
    return serialize(d);
}

std::string single_outcome(const std::string& severity, const std::string& code,
                           const std::string& diag) {
    std::vector<ValidationIssue> v;
    v.push_back(ValidationIssue(severity, code, diag));
    return operation_outcome(v);
}

void respond_outcome(server::Response& res, int status, const std::string& severity,
                     const std::string& code, const std::string& diag) {
    res.status(status);
    res.content_type(kFhirJsonMime);
    res.body(single_outcome(severity, code, diag));
}

void respond_json(server::Response& res, int status, const std::string& json) {
    res.status(status);
    res.content_type(kFhirJsonMime);
    res.body(json);
}

// Inject the server-owned id + meta (versionId, lastUpdated) into a parsed
// resource, preserving any existing meta.profile / security / tag.
void inject_meta(Document& d, const std::string& id, std::int64_t version,
                 const std::string& last_updated) {
    Document::AllocatorType& a = d.GetAllocator();

    d.RemoveMember("id");
    d.AddMember("id", Value(id.c_str(), a), a);

    Value* meta;
    Value::MemberIterator it = d.FindMember("meta");
    if (it != d.MemberEnd() && it->value.IsObject()) {
        meta = &it->value;
    } else {
        d.RemoveMember("meta");
        d.AddMember("meta", Value(rapidjson::kObjectType), a);
        meta = &d["meta"];
    }
    meta->RemoveMember("versionId");
    meta->AddMember("versionId", Value(std::to_string(version).c_str(), a), a);
    meta->RemoveMember("lastUpdated");
    meta->AddMember("lastUpdated", Value(last_updated.c_str(), a), a);
}


}  // namespace

// ─── outcome of a single resource op (reused by direct routes + Bundle) ──
namespace {
struct OpOutcome {
    int status = 200;
    std::string body;       // resource JSON or OperationOutcome
    std::string location;   // set on 201
};
}  // namespace

FhirService::FhirService(server::Router& router, const Config& cfg,
                         database::Database& db, const jwt::Jwt& jwt)
    : store_(db), jwt_(jwt) {
    base_url_path_ = cfg.uri_prefix + "/fhir";
    register_routes(router);
}

void FhirService::register_routes(server::Router& router) {
    using server::Request;
    using server::Response;

    // Public capability discovery.
    router.get("/fhir/metadata", [this](const Request& q, Response& s) { handle_metadata(q, s); });

    // Authenticated, user-scoped resource routes.
    server::HttpHandler type_h = [this](const Request& q, Response& s) { handle_type(q, s); };
    server::HttpHandler inst_h = [this](const Request& q, Response& s) { handle_instance(q, s); };
    server::HttpHandler txn_h  = [this](const Request& q, Response& s) { handle_transaction(q, s); };

    router.post("/fhir", server::require_auth(jwt_, txn_h));
    router.get("/fhir/{type}", server::require_auth(jwt_, type_h));
    router.post("/fhir/{type}", server::require_auth(jwt_, type_h));
    router.get("/fhir/{type}/{id}", server::require_auth(jwt_, inst_h));
    router.put("/fhir/{type}/{id}", server::require_auth(jwt_, inst_h));
    router.del("/fhir/{type}/{id}", server::require_auth(jwt_, inst_h));
}

void FhirService::handle_metadata(const server::Request&, server::Response& res) {
    Document d;
    d.SetObject();
    Document::AllocatorType& a = d.GetAllocator();
    d.AddMember("resourceType", "CapabilityStatement", a);
    d.AddMember("status", "active", a);
    d.AddMember("kind", "instance", a);
    d.AddMember("fhirVersion", Value(kFhirVersionR4, a), a);
    Value fmt(rapidjson::kArrayType);
    fmt.PushBack(Value(kFhirJsonMime, a), a);
    d.AddMember("format", fmt, a);

    Value rest(rapidjson::kArrayType);
    Value server_obj(rapidjson::kObjectType);
    server_obj.AddMember("mode", "server", a);
    Value resources(rapidjson::kArrayType);
    const std::vector<std::string>& types = known_resource_types();
    const char* interactions[] = {"read", "create", "update", "delete", "search-type"};
    for (size_t i = 0; i < types.size(); ++i) {
        Value r(rapidjson::kObjectType);
        r.AddMember("type", Value(types[i].c_str(), a), a);
        Value inter(rapidjson::kArrayType);
        for (size_t k = 0; k < 5; ++k) {
            Value io(rapidjson::kObjectType);
            io.AddMember("code", Value(interactions[k], a), a);
            inter.PushBack(io, a);
        }
        r.AddMember("interaction", inter, a);
        resources.PushBack(r, a);
    }
    server_obj.AddMember("resource", resources, a);
    rest.PushBack(server_obj, a);
    d.AddMember("rest", rest, a);

    respond_json(res, 200, serialize(d));
}

namespace {

// Validate + parse a request body into `doc`. On failure fills `err` (an
// OperationOutcome) and returns the HTTP status to send; returns 0 on success.
int parse_body(const std::string& body, Document& doc, std::string& err) {
    if (body.empty()) {
        err = single_outcome("error", "structure", "request body is empty");
        return 400;
    }
    doc.Parse(body.c_str(), body.size());
    if (doc.HasParseError()) {
        err = single_outcome("error", "structure", "request body is not valid JSON");
        return 400;
    }
    return 0;
}

}  // namespace

void FhirService::handle_type(const server::Request& req, server::Response& res) {
    const std::string type = req.path_params.count("type") ? req.path_params.at("type") : "";
    if (!is_well_formed_resource_type(type)) {
        respond_outcome(res, 400, "error", "value", "invalid resource type: " + type);
        return;
    }

    if (req.method == "POST") {
        // ── create ──
        Document doc;
        std::string err;
        int st = parse_body(req.body, doc, err);
        if (st) { respond_json(res, st, err); return; }

        std::string body_type, body_id;
        std::vector<ValidationIssue> issues = validate_resource(doc, &body_type, &body_id);
        if (!issues.empty()) { respond_json(res, 422, operation_outcome(issues)); return; }
        if (body_type != type) {
            respond_outcome(res, 400, "error", "value",
                            "resourceType '" + body_type + "' does not match endpoint '" + type + "'");
            return;
        }

        StoredResource sr;
        sr.type = type;
        sr.id = gen_uuid();              // server-assigned; any client id is ignored
        sr.version_id = 1;
        sr.updated_at = platform::now_unix_ms();
        sr.deleted = false;
        inject_meta(doc, sr.id, sr.version_id, iso8601_from_unix_ms(sr.updated_at));
        sr.content = serialize(doc);
        store_.upsert(req.user_id, sr);

        res.header("Location", base_url_path_ + "/" + type + "/" + sr.id + "/_history/1");
        res.header("ETag", "W/\"1\"");
        respond_json(res, 201, sr.content);
        return;
    }

    // ── search (GET) ──
    std::string id_filter = req.query_get("_id");
    int count = 50;
    std::string cstr = req.query_get("_count");
    if (!cstr.empty()) { count = std::atoi(cstr.c_str()); }
    if (count <= 0) count = 50;
    if (count > 200) count = 200;
    int offset = 0;
    std::string ostr = req.query_get("_offset");
    if (!ostr.empty()) { offset = std::atoi(ostr.c_str()); if (offset < 0) offset = 0; }

    std::int64_t total = 0;
    std::vector<StoredResource> hits = store_.search(req.user_id, type, id_filter, count, offset, total);

    Document d;
    d.SetObject();
    Document::AllocatorType& a = d.GetAllocator();
    d.AddMember("resourceType", "Bundle", a);
    d.AddMember("type", "searchset", a);
    d.AddMember("total", static_cast<int64_t>(total), a);
    Value entries(rapidjson::kArrayType);
    for (size_t i = 0; i < hits.size(); ++i) {
        Value entry(rapidjson::kObjectType);
        std::string full = base_url_path_ + "/" + type + "/" + hits[i].id;
        entry.AddMember("fullUrl", Value(full.c_str(), a), a);
        Document rdoc;
        rdoc.Parse(hits[i].content.c_str(), hits[i].content.size());
        if (!rdoc.HasParseError()) {
            Value rv;
            rv.CopyFrom(rdoc, a);
            entry.AddMember("resource", rv, a);
        }
        Value search(rapidjson::kObjectType);
        search.AddMember("mode", "match", a);
        entry.AddMember("search", search, a);
        entries.PushBack(entry, a);
    }
    d.AddMember("entry", entries, a);
    respond_json(res, 200, serialize(d));
}

void FhirService::handle_instance(const server::Request& req, server::Response& res) {
    const std::string type = req.path_params.count("type") ? req.path_params.at("type") : "";
    const std::string id   = req.path_params.count("id") ? req.path_params.at("id") : "";
    if (!is_well_formed_resource_type(type)) {
        respond_outcome(res, 400, "error", "value", "invalid resource type: " + type);
        return;
    }
    if (!is_valid_fhir_id(id)) {
        respond_outcome(res, 400, "error", "value", "invalid resource id: " + id);
        return;
    }

    if (req.method == "GET") {
        StoredResource sr;
        if (!store_.get(req.user_id, type, id, sr)) {
            respond_outcome(res, 404, "error", "not-found", type + "/" + id + " not found");
            return;
        }
        if (sr.deleted) {
            respond_outcome(res, 410, "error", "deleted", type + "/" + id + " was deleted");
            return;
        }
        res.header("ETag", "W/\"" + std::to_string(sr.version_id) + "\"");
        res.header("Last-Modified", iso8601_from_unix_ms(sr.updated_at));
        respond_json(res, 200, sr.content);
        return;
    }

    if (req.method == "PUT") {
        Document doc;
        std::string err;
        int st = parse_body(req.body, doc, err);
        if (st) { respond_json(res, st, err); return; }

        std::string body_type, body_id;
        std::vector<ValidationIssue> issues = validate_resource(doc, &body_type, &body_id);
        if (!issues.empty()) { respond_json(res, 422, operation_outcome(issues)); return; }
        if (body_type != type) {
            respond_outcome(res, 400, "error", "value",
                            "resourceType '" + body_type + "' does not match endpoint '" + type + "'");
            return;
        }
        if (!body_id.empty() && body_id != id) {
            respond_outcome(res, 400, "error", "value",
                            "resource id '" + body_id + "' does not match URL id '" + id + "'");
            return;
        }

        StoredResource prior;
        bool exists = store_.get(req.user_id, type, id, prior) && !prior.deleted;
        std::int64_t version = exists ? prior.version_id + 1 : 1;

        StoredResource sr;
        sr.type = type;
        sr.id = id;
        sr.version_id = version;
        sr.updated_at = platform::now_unix_ms();
        sr.deleted = false;
        inject_meta(doc, id, version, iso8601_from_unix_ms(sr.updated_at));
        sr.content = serialize(doc);
        store_.upsert(req.user_id, sr);

        res.header("ETag", "W/\"" + std::to_string(version) + "\"");
        res.header("Last-Modified", iso8601_from_unix_ms(sr.updated_at));
        if (!exists) {
            res.header("Location", base_url_path_ + "/" + type + "/" + id + "/_history/" +
                                       std::to_string(version));
            respond_json(res, 201, sr.content);
        } else {
            respond_json(res, 200, sr.content);
        }
        return;
    }

    if (req.method == "DELETE") {
        // Idempotent: 204 whether or not the resource was present/already gone.
        StoredResource prior;
        if (store_.get(req.user_id, type, id, prior) && !prior.deleted) {
            store_.soft_delete(req.user_id, type, id, prior.version_id + 1, platform::now_unix_ms());
        }
        res.status(204);
        return;
    }

    respond_outcome(res, 405, "error", "not-supported", "method not allowed");
}

void FhirService::handle_transaction(const server::Request& req, server::Response& res) {
    Document bundle;
    std::string err;
    int st = parse_body(req.body, bundle, err);
    if (st) { respond_json(res, st, err); return; }

    if (!bundle.IsObject() || !bundle.HasMember("resourceType") ||
        !bundle["resourceType"].IsString() ||
        std::string(bundle["resourceType"].GetString()) != "Bundle") {
        respond_outcome(res, 400, "error", "structure", "body is not a Bundle");
        return;
    }
    std::string btype = bundle.HasMember("type") && bundle["type"].IsString()
                            ? bundle["type"].GetString() : "";
    // We process entries independently (no atomic rollback — see header note), so
    // batch and transaction are handled the same way; the response type mirrors
    // the request type.
    std::string resp_type = (btype == "transaction") ? "transaction-response" : "batch-response";

    Document out;
    out.SetObject();
    Document::AllocatorType& a = out.GetAllocator();
    out.AddMember("resourceType", "Bundle", a);
    out.AddMember("type", Value(resp_type.c_str(), a), a);
    Value out_entries(rapidjson::kArrayType);

    if (bundle.HasMember("entry") && bundle["entry"].IsArray()) {
        const Value& entries = bundle["entry"];
        for (rapidjson::SizeType i = 0; i < entries.Size(); ++i) {
            const Value& entry = entries[i];
            std::string method, url;
            if (entry.IsObject() && entry.HasMember("request") && entry["request"].IsObject()) {
                const Value& rq = entry["request"];
                if (rq.HasMember("method") && rq["method"].IsString()) method = rq["method"].GetString();
                if (rq.HasMember("url") && rq["url"].IsString()) url = rq["url"].GetString();
            }

            // Synthesize a sub-request and reuse the instance/type handlers.
            server::Request sub;
            sub.user_id = req.user_id;
            sub.method = method;
            // url is "Type" or "Type/id" (relative). Split.
            std::string path = url;
            while (!path.empty() && path[0] == '/') path.erase(0, 1);
            size_t slash = path.find('/');
            std::string seg_type = slash == std::string::npos ? path : path.substr(0, slash);
            std::string seg_id = slash == std::string::npos ? "" : path.substr(slash + 1);
            sub.path_params["type"] = seg_type;
            if (!seg_id.empty()) sub.path_params["id"] = seg_id;
            if (entry.IsObject() && entry.HasMember("resource")) {
                sub.body = serialize(entry["resource"]);
            }

            server::Response sub_res;
            if (method == "POST" && seg_id.empty()) handle_type(sub, sub_res);
            else if (method == "GET" && seg_id.empty()) handle_type(sub, sub_res);
            else if (!seg_id.empty()) handle_instance(sub, sub_res);
            else {
                sub_res.status(400);
                sub_res.body(single_outcome("error", "value", "unsupported bundle entry: " + url));
            }

            Value oe(rapidjson::kObjectType);
            Value resp(rapidjson::kObjectType);
            resp.AddMember("status", Value(std::to_string(sub_res.status()).c_str(), a), a);
            for (size_t h = 0; h < sub_res.headers().size(); ++h) {
                if (sub_res.headers()[h].first == "Location")
                    resp.AddMember("location", Value(sub_res.headers()[h].second.c_str(), a), a);
            }
            oe.AddMember("response", resp, a);
            if (!sub_res.body().empty()) {
                Document rb;
                rb.Parse(sub_res.body().c_str(), sub_res.body().size());
                if (!rb.HasParseError()) {
                    Value rv;
                    rv.CopyFrom(rb, a);
                    oe.AddMember("resource", rv, a);
                }
            }
            out_entries.PushBack(oe, a);
        }
    }

    out.AddMember("entry", out_entries, a);
    respond_json(res, 200, serialize(out));
}

}}  // namespace mirobody::fhir

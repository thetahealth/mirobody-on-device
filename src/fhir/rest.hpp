#pragma once

// RESTful FHIR R4 endpoint.
//
// Constructing the service wires its routes onto the Router, mirroring the
// McpService / ChatService pattern:
//
//     fhir::FhirService fhir_svc(router, cfg, db, jwt);
//
// `cfg`, `db`, and `jwt` are borrowed (not owned) and must outlive the running
// server. Resource routes are guarded by a bearer JWT (medical data) and scoped
// to the authenticated user; `GET /fhir/metadata` is public discovery.
//
// Routes (served under HTTP_URI_PREFIX when set):
//
//   GET    /fhir/metadata        CapabilityStatement
//   POST   /fhir                 batch/transaction Bundle
//   GET    /fhir/{type}          search (searchset Bundle); params _id/_count/_offset
//   POST   /fhir/{type}          create (server-assigned id) -> 201
//   GET    /fhir/{type}/{id}     read -> 200 / 404 / 410 (deleted)
//   PUT    /fhir/{type}/{id}     update or create-with-id -> 200 / 201
//   DELETE /fhir/{type}/{id}     delete (idempotent) -> 204
//
// Bodies are application/fhir+json; errors come back as an OperationOutcome.
// The generic-resource model means validation is structural only (see
// fhir/resource.hpp) and batch Bundles run as independent ops, not an atomic
// transaction (the DB layer has no multi-statement transaction wrapper yet).

#include "config/config.hpp"
#include "fhir/store.hpp"
#include "jwt/jwt.hpp"
#include "server/router.hpp"

namespace mirobody { namespace database { class Database; } }

namespace mirobody { namespace fhir {

class FhirService {
public:
    FhirService(server::Router& router, const Config& cfg,
                database::Database& db, const jwt::Jwt& jwt);

    FhirService(const FhirService&) = delete;
    FhirService& operator=(const FhirService&) = delete;

private:
    void register_routes(server::Router& router);

    void handle_metadata(const server::Request& req, server::Response& res);
    void handle_type(const server::Request& req, server::Response& res);      // GET search / POST create
    void handle_instance(const server::Request& req, server::Response& res);  // GET / PUT / DELETE
    void handle_transaction(const server::Request& req, server::Response& res);

    FhirStore       store_;
    const jwt::Jwt& jwt_;
    std::string     base_url_path_;  // uri_prefix + "/fhir", for Bundle fullUrl / Location
};

}}  // namespace mirobody::fhir

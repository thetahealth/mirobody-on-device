#pragma once

// HTTP surface for binding a user to a health-data vendor account and pulling
// their data. Constructing it wires the routes onto the Router, mirroring
// fhir::FhirService / user::UserService. `cfg`, `db`, and `jwt` are borrowed
// (not owned) and must outlive the running server. Every route requires a bearer
// JWT (medical data) and is scoped to the authenticated user.
//
// Routes (served under HTTP_URI_PREFIX when set):
//
//   POST /vendors/{id}/bind          submit the vendor account id  -> pending
//   POST /vendors/{id}/bind/verify   prove ownership               -> verified
//   GET  /vendors/{id}/data          fetch data (?domain=&start=&end=)
//
// Responses use the project JSON envelope ({"code","msg","data"}); {id} is a
// vendor key from the registry (src/health/vendor/registry.hpp). The binding mirrors
// email binding: bind records a PENDING link, verify proves ownership, and only
// a verified link may be used by /data (see health::fetch_for_user).

#include "config/config.hpp"
#include "health/vendor_link.hpp"
#include "jwt/jwt.hpp"
#include "server/router.hpp"

namespace mirobody { namespace database { class Database; } }

namespace mirobody { namespace health {

class VendorService {
public:
    VendorService(server::Router& router, const Config& cfg,
                  database::Database& db, const jwt::Jwt& jwt);

    VendorService(const VendorService&) = delete;
    VendorService& operator=(const VendorService&) = delete;

private:
    void register_routes(server::Router& router);

    void on_bind(const server::Request& req, server::Response& res);
    void on_bind_verify(const server::Request& req, server::Response& res);
    void on_fetch(const server::Request& req, server::Response& res);

    const Config&   cfg_;
    VendorLinkStore store_;
    const jwt::Jwt& jwt_;
};

}}  // namespace mirobody::health

#pragma once

// HTTP surface for binding a user to a health-data vendor account and pulling
// their data. Constructing it wires the routes onto the Router, mirroring
// fhir::FhirService / user::UserService. `cfg`, `db`, and `jwt` are borrowed
// (not owned) and must outlive the running server. Every route requires a bearer
// JWT (medical data) and is scoped to the authenticated user.
//
// Routes (served under HTTP_URI_PREFIX when set):
//
//   GET  /vendors                    list the user's connected vendors
//   GET  /vendors/{id}/authorize     -> {authorize_url} for the OAuth connect flow
//   GET  /vendors/callback           OAuth redirect back: exchange + store + verify
//   POST /vendors/{id}/bind          submit the vendor account id  -> pending
//   POST /vendors/{id}/bind/verify   prove ownership               -> verified
//   GET  /vendors/{id}/data          fetch data (?domain=&start=&end=)
//   POST /vendors/{id}/sync          fetch + map to FHIR Observations + persist
//   POST /vendors/{id}/unlink        revoke at vendor + delete the link + tokens
//
// Responses use the project JSON envelope ({"code","msg","data"}); {id} is a
// vendor key from the registry (src/health/vendor/registry.hpp). The binding mirrors
// email binding: bind records a PENDING link, verify proves ownership, and only
// a verified link may be used by /data (see health::fetch_for_user).

#include "config/config.hpp"
#include "health/vendor_link.hpp"
#include "jwt/jwt.hpp"
#include "server/router.hpp"

#include <atomic>
#include <mutex>
#include <string>
#include <thread>

namespace mirobody { namespace database { class Database; } }
namespace mirobody { namespace cache { class Cache; } }

namespace mirobody { namespace health {

class VendorService {
public:
    VendorService(server::Router& router, const Config& cfg,
                  database::Database& db, cache::Cache& cache, const jwt::Jwt& jwt);

    VendorService(const VendorService&) = delete;
    VendorService& operator=(const VendorService&) = delete;

private:
    void register_routes(server::Router& router);

    void on_list(const server::Request& req, server::Response& res);
    void on_icons(const server::Request& req, server::Response& res);
    void on_authorize(const server::Request& req, server::Response& res);
    void on_callback(const server::Request& req, server::Response& res);
    void on_bind(const server::Request& req, server::Response& res);
    void on_bind_verify(const server::Request& req, server::Response& res);
    void on_fetch(const server::Request& req, server::Response& res);
    void on_sync(const server::Request& req, server::Response& res);
    void on_unlink(const server::Request& req, server::Response& res);

    const Config&       cfg_;
    database::Database& db_;    // borrowed; a per-request FhirStore is built on it
    cache::Cache&       cache_; // OAuth connect state (state -> {user,vendor}, TTL)
    VendorLinkStore     store_;
    const jwt::Jwt&     jwt_;
};

}}  // namespace mirobody::health

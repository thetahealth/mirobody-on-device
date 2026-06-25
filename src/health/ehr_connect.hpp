#pragma once

// SMART-on-FHIR connect flow — lets a web (or app) client link a user's EHR
// (Epic, Oracle Health/Cerner, …) and pull their records. Here mirobody is an
// OAuth *client* of the EHR, distinct from src/oauth where it is the
// authorization *server*. The browser drives the redirect; this service builds
// the authorize URL (PKCE), handles the OAuth callback + token exchange, and then
// uses the `ehr` vendor client (src/health/vendor/ehr/) to fetch FHIR and persist
// each Observation per user via the FHIR store.
//
// Routes (served under HTTP_URI_PREFIX):
//   GET  /health/ehr/providers?q=&source=        list tenants from the directory
//   POST /health/ehr/authorize  {fhir_base_url}   -> {authorize_url}      (auth)
//   GET  /health/ehr/callback?code=&state=        token exchange, 302 to the app
//   POST /health/ehr/sync                          fetch + store Observations (auth)
//
// State between authorize and callback (code_verifier, tenant base URL, token
// endpoint, user id) lives in the cache keyed by the OAuth `state`, single-use
// with a short TTL — mirroring how src/oauth stores authorization codes. The
// resulting access token is cached per user for its lifetime.

#include "config/config.hpp"
#include "fhir/store.hpp"
#include "jwt/jwt.hpp"
#include "server/router.hpp"

namespace mirobody { namespace database { class Database; } }
namespace mirobody { namespace cache { class Cache; } }

namespace mirobody { namespace health {

class EhrConnectService {
public:
    EhrConnectService(server::Router& router, const Config& cfg,
                      database::Database& db, cache::Cache& cache, const jwt::Jwt& jwt);

    EhrConnectService(const EhrConnectService&) = delete;
    EhrConnectService& operator=(const EhrConnectService&) = delete;

private:
    void register_routes(server::Router& router);

    void on_providers(const server::Request& req, server::Response& res);
    void on_authorize(const server::Request& req, server::Response& res);
    void on_callback(const server::Request& req, server::Response& res);
    void on_sync(const server::Request& req, server::Response& res);

    const Config&   cfg_;
    fhir::FhirStore fhir_store_;
    cache::Cache&   cache_;
    const jwt::Jwt& jwt_;
};

}}  // namespace mirobody::health

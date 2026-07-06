#pragma once

#include "chat/service.hpp"
#include "circle/service.hpp"
#include "config/config.hpp"
#include "fhir/rest.hpp"
#include "health/ehr_connect.hpp"
#include "health/vendor_service.hpp"
#include "health/werun.hpp"
#include "memory/memory.hpp"
#include "mcp/service.hpp"
#include "oauth/service.hpp"
#include "jwt/apple.hpp"
#include "jwt/firebase.hpp"
#include "jwt/jwt.hpp"
#include "server/router.hpp"
#include "storage/storage.hpp"
#include "user/service.hpp"

#include <atomic>
#include <memory>
#include <string>
#include <thread>

struct lws_context;
struct lws_protocol_vhost_options;

namespace mirobody {

class Server {
public:
    explicit Server(Config cfg);
    ~Server();

    Server(const Server&) = delete;
    Server& operator=(const Server&) = delete;

    bool start();
    void stop();
    void run_blocking();

    bool start_in_background();
    void stop_and_join();

    bool is_running() const { return running_.load(); }
    const Config& config() const { return cfg_; }

private:
    // Releases the router, the services that registered onto it, and the
    // backing resources they borrow — in dependency order (router first,
    // backing resources last). Safe to call with anything already null.
    void teardown();

    // Register the infrastructure HTTP routes that belong to no domain service
    // (currently the unauthenticated /api/health probe). Authenticated routes
    // are registered by their respective services (chat/user/mcp/...).
    void register_http_routes();

    Config cfg_;
    // Backing resources, owned for the lifetime of a running server. Declared
    // before router_ so they are destroyed *after* it (members tear down in
    // reverse): the router holds handlers that capture user_service_, which in
    // turn borrows db_/cache_/jwt_.
    std::unique_ptr<database::Database> db_;
    std::unique_ptr<cache::Cache>       cache_;
    std::unique_ptr<storage::Storage>   storage_;
    // Long-term memory store. Borrowed by chat_service_ and mcp_service_, so it
    // is declared before them (destroyed after) and after db_, which it borrows.
    std::unique_ptr<memory::Memory>     memory_;
    std::unique_ptr<jwt::Jwt>           jwt_;   // JwtHs256 (default) or JwtRs256 when JWT_PRIVATE_KEY is set
    std::string                         jwks_json_;   // public JWKS to publish; empty under HS256
    std::unique_ptr<jwt::FirebaseTokenValidator> firebase_;
    std::unique_ptr<jwt::AppleTokenValidator>    apple_;
    std::unique_ptr<user::UserService>  user_service_;
    std::unique_ptr<chat::ChatService>  chat_service_;
    std::unique_ptr<circle::CircleService> circle_service_;  // care circles; modern backends only (null on legacy)
    std::unique_ptr<mcp::McpService>    mcp_service_;
    std::unique_ptr<oauth::OAuthService> oauth_service_;
    std::unique_ptr<fhir::FhirService>  fhir_service_;
    std::unique_ptr<health::VendorService> vendor_service_;
    std::unique_ptr<health::EhrConnectService> ehr_connect_service_;
    std::unique_ptr<health::WeRunService> werun_service_;
    std::unique_ptr<server::Router>     router_;
    // Security headers handed to lws via info.headers. lws keeps these pointers
    // for the vhost lifetime rather than copying, so the CSP string and the
    // options array must outlive context_ — hence they live here, not on the
    // stack of start(). The CSP embeds the Firebase auth domain, which is
    // derived from cfg_.firebase_project_id at startup.
    std::string csp_header_value_;
    std::unique_ptr<lws_protocol_vhost_options[]> sec_headers_;
    lws_context* context_ = nullptr;
    std::atomic<bool> stop_flag_{false};
    std::atomic<bool> running_{false};
    std::thread thread_;
};

}

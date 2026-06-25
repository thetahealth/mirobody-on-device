#if defined(__APPLE__)
#include <TargetConditionals.h>
#endif

#if defined(__APPLE__) && TARGET_OS_IPHONE

#include "mirobody.h"

#include "client/http_client.hpp"
#include "config/config.hpp"
#include "platform/log.hpp"
#include "server/server.hpp"

#include <libwebsockets.h>

#include <cstdint>
#include <exception>
#include <memory>
#include <mutex>
#include <string>
#include <utility>

namespace {

std::once_flag g_curl_init_flag;

void ensure_curl_init() {
    std::call_once(g_curl_init_flag, [] {
        mirobody::client::HttpClient::global_init();
    });
}

void lws_log_to_ios(int /*level*/, const char* line) {
    mirobody::platform::log_info("%s", line);
}

std::string c_to_std(const char* s) {
    return s ? std::string(s) : std::string{};
}

}

//------------------------------------------------------------------------------

extern "C" mirobody_server_t* mirobody_start(
        const char* config_path,
        const char* /*data_dir*/) {

    ensure_curl_init();

    int log_mask = LLL_ERR | LLL_WARN;
    lws_set_log_level(log_mask, lws_log_to_ios);

    std::string cfg_path = c_to_std(config_path);

    mirobody::Config cfg;
    try {
        if (!cfg_path.empty()) {
            cfg = mirobody::load_config(cfg_path);
        } else {
            cfg = mirobody::load_config();
        }
    } catch (const std::exception& e) {
        mirobody::platform::log_error("config error: %s", e.what());
        return nullptr;
    }

    // LLM keys and the listen port come from the config (and its env-var
    // fallbacks: OPENAI_API_KEY / GOOGLE_API_KEY / HTTP_PORT).
    // iOS hosts always reach mirobody over loopback; ignore any 0.0.0.0 default
    // so we don't trip the local-network privacy prompt.
    if (cfg.listen_addr.empty() || cfg.listen_addr == "0.0.0.0") {
        cfg.listen_addr = "127.0.0.1";
    }

    auto server = std::unique_ptr<mirobody::Server>(new mirobody::Server(std::move(cfg)));
    if (!server->start_in_background()) {
        mirobody::platform::log_error("server failed to start");
        return nullptr;
    }
    return reinterpret_cast<mirobody_server_t*>(server.release());
}

//------------------------------------------------------------------------------

extern "C" void mirobody_stop(mirobody_server_t* handle) {
    if (!handle) return;
    auto* server = reinterpret_cast<mirobody::Server*>(handle);
    server->stop_and_join();
    delete server;
}

//------------------------------------------------------------------------------

extern "C" int mirobody_is_running(mirobody_server_t* handle) {
    if (!handle) return 0;
    auto* server = reinterpret_cast<mirobody::Server*>(handle);
    return server->is_running() ? 1 : 0;
}

//------------------------------------------------------------------------------

extern "C" int mirobody_listen_port(mirobody_server_t* handle) {
    if (!handle) return -1;
    auto* server = reinterpret_cast<mirobody::Server*>(handle);
    return static_cast<int>(server->config().listen_port);
}

#endif

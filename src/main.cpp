#include "client/http_client.hpp"
#include "config/config.hpp"
#include "database/schema.hpp"
#include "platform/log.hpp"
#include "sentry.hpp"
#include "server/server.hpp"
#include <optional>

#include <libwebsockets.h>

#include <atomic>
#include <cctype>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <stdexcept>
#include <string>

//------------------------------------------------------------------------------
// Signal handling
//------------------------------------------------------------------------------

namespace {

mirobody::Server* g_server = nullptr;
std::atomic<bool> g_stop{false};

#ifdef _WIN32
BOOL WINAPI ctrl_handler(DWORD type) {
    if (type == CTRL_C_EVENT || type == CTRL_BREAK_EVENT || type == CTRL_CLOSE_EVENT) {
        g_stop.store(true);
        if (g_server) g_server->stop();
        return TRUE;
    }
    return FALSE;
}
#else
void ctrl_handler(int) {
    g_stop.store(true);
    if (g_server) g_server->stop();
}
#endif

void install_signal_handlers() {
#ifdef _WIN32
    SetConsoleCtrlHandler(ctrl_handler, TRUE);
#else
    std::signal(SIGINT, ctrl_handler);
    std::signal(SIGTERM, ctrl_handler);
    std::signal(SIGPIPE, SIG_IGN);
#endif
}

//------------------------------------------------------------------------------
// libwebsockets log formatting
//------------------------------------------------------------------------------

void lws_log_to_stderr(int level, const char* line) {
    (void)level;
    // libwebsockets prefixes each line with "[YYYY/MM/DD HH:MM:SS:NNNN]" (4-digit
    // ms, colon-separated). Rewrite to "[YYYY-MM-DD HH:MM:SS.NNN]" — ISO 8601
    // date separators, dot before ms, 3-digit ms — to match platform/log.cpp.
    auto is_digit = [](char c) { return std::isdigit(static_cast<unsigned char>(c)) != 0; };
    if (line && std::strlen(line) >= 26
        && line[0] == '[' && line[5] == '/' && line[8] == '/' && line[11] == ' '
        && line[14] == ':' && line[17] == ':' && line[20] == ':'
        && is_digit(line[21]) && is_digit(line[22])
        && is_digit(line[23]) && is_digit(line[24])
        && line[25] == ']') {
        char buf[32];
        std::snprintf(buf, sizeof(buf), "[%.4s-%.2s-%.2s %.8s.%.3s]",
            line + 1, line + 6, line + 9, line + 12, line + 21);
        std::fputs(buf, stderr);
        std::fputs(line + 26, stderr);   // " <level>: <msg>\n"
    } else {
        std::fputs(line, stderr);
    }
}

}

//------------------------------------------------------------------------------
// Entry point
//------------------------------------------------------------------------------

int main(int argc, char** argv) {
    install_signal_handlers();

    std::optional<std::string> yaml_path;
    for (int i = 1; i < argc; ++i) {
        std::string arg(argv[i]);
        if (arg == "/?" || arg == "-h" || arg == "--help") {
            std::printf(
                "Usage: %s [--config <path>]\n"
                "\n"
                "  --config <path>     YAML config file. If omitted, falls back to\n"
                "                      MIROBODY_CONFIG env, then ./config.yml in the\n"
                "                      current directory, then built-in defaults.\n"
                "  /? | -h | --help    Show this help and exit.\n"
                "\n"
                "Environment variables (see README \"Runtime\" / \"Remote config\"):\n"
                "  MIROBODY_CONFIG          Local YAML path when --config is absent.\n"
                "  CONFIG_SERVER + CONFIG_TOKEN + ENV   Pull remote YAML before the local file.\n"
                "  CONFIG_ENCRYPTION_KEY    Fernet key to decrypt gAAAA-prefixed values.\n",
                argv[0]);
            return 0;
        }
        if (arg == "--config" && i + 1 < argc) {
            yaml_path = argv[i + 1];
            ++i;
        }
    }

    mirobody::Config cfg;
    try {
        cfg = mirobody::load_config(yaml_path);
    } catch (const std::exception& e) {
        mirobody::platform::log_error("config error: %s", e.what());
        return -1;
    }
    cfg.print();

    // Gate the application logger (platform::log_*) on LOG_LEVEL so log_debug —
    // e.g. the per-request HTTP access log in the router — only emits in debug.
    mirobody::platform::set_log_level(cfg.log_level.c_str());

    // Bring up crash/error reporting before any of the real work. The guard
    // flushes and closes Sentry on every return below.
    mirobody::SentryGuard sentry_guard;
    sentry_guard.active = mirobody::init_sentry(cfg);

    mirobody::client::HttpClient::global_init();

    int log_mask = LLL_ERR | LLL_WARN;
    if (cfg.log_level == "debug") log_mask |= LLL_NOTICE | LLL_INFO | LLL_DEBUG;
    lws_set_log_level(log_mask, lws_log_to_stderr);

    mirobody::Server server(std::move(cfg));
    g_server = &server;

    if (!server.start()) {
        mirobody::client::HttpClient::global_cleanup();
        return -2;
    }
    server.run_blocking();

    mirobody::client::HttpClient::global_cleanup();
    return 0;
}

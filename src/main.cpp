#include "client/http_client.hpp"
#include "config/config.hpp"
#include "database/schema.hpp"
#include "platform/log.hpp"
#include "server/server.hpp"

#include <libwebsockets.h>

#include <sentry.h>

#include <atomic>
#include <cctype>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <stdexcept>
#include <string>

#ifndef _WIN32
#include <unistd.h>   // readlink, for executable_dir()
#endif

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

//------------------------------------------------------------------------------
// Sentry lifetime guard
//------------------------------------------------------------------------------

// Flushes and shuts down Sentry on scope exit so every return path out of
// main() (config error, db init failure, clean shutdown) closes it exactly
// once. Inactive unless init_sentry() actually brought a session up.
struct SentryGuard {
    bool active = false;
    ~SentryGuard() {
        if (active) sentry_close();
    }
};

// Trims ASCII whitespace from both ends.
std::string trim(std::string s) {
    const auto b = s.find_first_not_of(" \t\r\n");
    if (b == std::string::npos) return std::string();
    const auto f = s.find_last_not_of(" \t\r\n");
    return s.substr(b, f - b + 1);
}

// Sentry release identifier, mirroring the Python MCP server's
// _get_sentry_release(): prefer an explicit SENTRY_RELEASE (k8s bakes it into
// the image, e.g. "test-abc1234"); otherwise fall back to "local-<short_sha>"
// from the git SHA captured at build time (the deployed binary has no source
// tree / git to query at runtime, unlike the Python process). Empty when
// neither is available — the caller then leaves the release unset, as Python
// returns None.
std::string sentry_release(const mirobody::Config& cfg) {
    const std::string explicit_release = trim(cfg.store.get_str("SENTRY_RELEASE"));
    if (!explicit_release.empty() && explicit_release != "unknown") {
        return explicit_release;
    }
    const std::string sha = MIROBODY_GIT_SHA;   // "" when git was unavailable at build time
    return sha.empty() ? std::string() : ("local-" + sha);
}

// Absolute directory of the running executable, used to locate the
// crashpad_handler that CMake ships next to the binary. Returns empty on
// failure, in which case the caller falls back to SENTRY_HANDLER_PATH or skips
// the crashpad backend. Independent of the working directory (unlike argv[0]).
std::string executable_dir() {
#ifdef _WIN32
    std::wstring buf(MAX_PATH, L'\0');
    for (;;) {
        const DWORD n = GetModuleFileNameW(nullptr, &buf[0], static_cast<DWORD>(buf.size()));
        if (n == 0) return std::string();
        if (n < buf.size()) { buf.resize(n); break; }
        buf.resize(buf.size() * 2);   // path was truncated; grow and retry
    }
    const int len = WideCharToMultiByte(CP_UTF8, 0, buf.c_str(), -1, nullptr, 0, nullptr, nullptr);
    if (len <= 0) return std::string();
    std::string path(static_cast<std::size_t>(len - 1), '\0');
    WideCharToMultiByte(CP_UTF8, 0, buf.c_str(), -1, &path[0], len, nullptr, nullptr);
#else
    char raw[4096];
    const ssize_t n = ::readlink("/proc/self/exe", raw, sizeof(raw) - 1);
    if (n <= 0) return std::string();
    raw[n] = '\0';
    std::string path(raw);
#endif
    const std::string::size_type slash = path.find_last_of("/\\");
    return slash == std::string::npos ? std::string() : path.substr(0, slash);
}

// True when `path` names an existing, openable file. Used to confirm the
// crashpad_handler is present before pointing Sentry at it.
bool file_exists(const std::string& path) {
    if (path.empty()) return false;
    if (FILE* fh = std::fopen(path.c_str(), "rb")) { std::fclose(fh); return true; }
    return false;
}

// Initialises Sentry, mirroring the Python MCP server's bootstrap. Returns true
// when a session was started. ENV (the deployment selector) is normalised to
// upper case with an "unknown" default; ENV=TEST-INLOCAL opts out entirely, and
// a missing SENTRY_DSN leaves Sentry uninitialised so self-hosted
// runs report nothing unless explicitly opted in.
bool init_sentry(const mirobody::Config& cfg) {
    const char* raw_env = std::getenv("ENV");
    std::string env = "unknown";
    if (raw_env) {
        const std::string raw = trim(raw_env);
        if (!raw.empty()) env = raw;
    }
    for (char& c : env) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    mirobody::platform::log_debug("sentry: ENV raw=%s -> normalised environment=%s",
        raw_env ? raw_env : "<unset>", env.c_str());

    const std::string dsn = cfg.store.get_str("SENTRY_DSN");
    if (dsn.empty()) {
        mirobody::platform::log_warn("sentry: SENTRY_DSN is not set, not initialised");
        return false;
    }
    // DSN is a secret, so log only that it is present and its length, never the value.
    mirobody::platform::log_debug("sentry: SENTRY_DSN is set (%zu chars)", dsn.size());

    const std::string db_path = cfg.store.get_str("SENTRY_DATABASE_PATH", ".sentry-native");
    const std::string release = sentry_release(cfg);
    // Case-insensitive, matching platform::set_log_level (which lowercases before
    // comparing). A raw `cfg.log_level == "debug"` would leave Sentry's internal
    // logging off for LOG_LEVEL=DEBUG/Debug even though the app logger is at debug,
    // hiding the SDK's own diagnostics when sentry_init fails.
    std::string lvl = cfg.log_level;
    for (char& c : lvl) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    const int    debug_flag         = (lvl == "debug") ? 1 : 0;
    const double traces_sample_rate = 1.0;
    mirobody::platform::log_debug("sentry: release resolved to %s",
        release.empty() ? "<none> (left unset, as Python returns None)" : release.c_str());

    // Crashpad backend: sentry_init launches a separate crashpad_handler process
    // and fails init ("invalid handler_path") when the path is unset — the SDK
    // does not auto-discover it. Prefer an explicit SENTRY_HANDLER_PATH, else the
    // handler that CMake copies next to this binary.
    std::string handler_path = cfg.store.get_str("SENTRY_HANDLER_PATH");
    if (handler_path.empty()) {
        const std::string dir = executable_dir();
        if (!dir.empty()) {
#ifdef _WIN32
            handler_path = dir + "\\crashpad_handler.exe";
#else
            handler_path = dir + "/crashpad_handler";
#endif
        }
    }
    const bool have_handler = file_exists(handler_path);

    sentry_options_t* options = sentry_options_new();
    if (!options) {
        mirobody::platform::log_error("sentry: sentry_options_new() returned null; cannot initialise");
        return false;
    }
    mirobody::platform::log_debug("sentry: options allocated; applying configuration");
    sentry_options_set_dsn(options, dsn.c_str());
    sentry_options_set_database_path(options, db_path.c_str());
    sentry_options_set_environment(options, env.c_str());
    sentry_options_set_traces_sample_rate(options, traces_sample_rate);
    sentry_options_set_debug(options, debug_flag);
    if (!release.empty()) {
        sentry_options_set_release(options, release.c_str());
    }
    if (have_handler) {
        sentry_options_set_handler_path(options, handler_path.c_str());
    } else {
        mirobody::platform::log_warn(
            "sentry: crashpad_handler not found (looked for '%s'); the crashpad "
            "backend cannot start, so sentry_init will fail. Set SENTRY_HANDLER_PATH "
            "or ship crashpad_handler next to the binary.",
            handler_path.empty() ? "<undetermined>" : handler_path.c_str());
    }

    mirobody::platform::log_debug(
        "sentry: options set (environment=%s, database_path=%s, traces_sample_rate=%.2f, "
        "debug=%d, release=%s, dsn=set, handler_path=%s); calling sentry_init()",
        env.c_str(), db_path.c_str(), traces_sample_rate, debug_flag,
        release.empty() ? "<auto>" : release.c_str(),
        have_handler ? handler_path.c_str() : "<none>");

    const int rc = sentry_init(options);
    mirobody::platform::log_debug("sentry: sentry_init() returned %d", rc);
    if (rc != 0) {
        mirobody::platform::log_error(
            "sentry: init failed (sentry_init rc=%d); continuing without crash reporting", rc);
        return false;
    }
    mirobody::platform::log_info(
        "sentry: initialised (environment=%s, release=%s)",
        env.c_str(), release.empty() ? "<auto>" : release.c_str());
    return true;
}

}

//------------------------------------------------------------------------------
// Entry point
//------------------------------------------------------------------------------

int main(int argc, char** argv) {
    install_signal_handlers();

    mirobody::optional<std::string> yaml_path;
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
    SentryGuard sentry_guard;
    sentry_guard.active = init_sentry(cfg);

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

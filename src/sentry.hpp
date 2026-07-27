#pragma once

// Sentry crash/error-reporting bootstrap for the standalone server executable.
//
// Everything main() needs to stand Sentry up and tear it down, lifted out of
// src/main.cpp so the entry point stays about lifecycle, not SDK plumbing.
// Header-only on purpose: src/main.cpp is the ONLY translation unit linked
// against sentry-native (see CMakeLists.txt — crash reporting lives only in the
// desktop entry point), and it is also the TU compiled with MIROBODY_GIT_SHA
// (the release fallback), so nothing else can or should include this.
//
// Public surface (namespace mirobody):
//   SentryGuard   — RAII flush + close on scope exit.
//   init_sentry() — bring a session up from Config; returns whether it started.
// The rest are inline helpers under mirobody::detail.
//
// sentry-native is OPTIONAL (find_package(sentry CONFIG QUIET) in CMakeLists.txt):
// cross-compiled sysroots — Android, iOS, HarmonyOS — don't ship it. When it is
// absent MIROBODY_HAS_SENTRY is 0 and the two public entry points below degrade
// to no-ops, so main.cpp compiles and runs unchanged, just without reporting.

#include "config/config.hpp"
#include "platform/log.hpp"

#ifndef MIROBODY_HAS_SENTRY
#  define MIROBODY_HAS_SENTRY 0
#endif

#if !MIROBODY_HAS_SENTRY

namespace mirobody {

// No-op stand-ins: same surface, nothing behind it.
struct SentryGuard {
    bool active = false;
};

inline bool init_sentry(const Config&) {
    platform::log_info("sentry: not built in; crash reporting disabled");
    return false;
}

}   // namespace mirobody

#else

#include <sentry.h>

#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

#ifdef _WIN32
#include <windows.h>  // GetModuleFileNameW / WideCharToMultiByte, for executable_dir()
#else
#include <unistd.h>   // readlink, for executable_dir()
#endif

namespace mirobody {

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

namespace detail {

// Trims ASCII whitespace from both ends.
inline std::string trim(std::string s) {
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
inline std::string sentry_release(const Config& cfg) {
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
inline std::string executable_dir() {
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
inline bool file_exists(const std::string& path) {
    if (path.empty()) return false;
    if (FILE* fh = std::fopen(path.c_str(), "rb")) { std::fclose(fh); return true; }
    return false;
}

}  // namespace detail

//------------------------------------------------------------------------------
// Initialisation
//------------------------------------------------------------------------------

// Initialises Sentry, mirroring the Python MCP server's bootstrap. Returns true
// when a session was started. ENV (the deployment selector) is normalised to
// upper case with an "unknown" default; ENV=TEST-INLOCAL opts out entirely, and
// a missing SENTRY_DSN leaves Sentry uninitialised so self-hosted
// runs report nothing unless explicitly opted in.
inline bool init_sentry(const Config& cfg) {
    const char* raw_env = std::getenv("ENV");
    std::string env = "unknown";
    if (raw_env) {
        const std::string raw = detail::trim(raw_env);
        if (!raw.empty()) env = raw;
    }
    for (char& c : env) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    platform::log_debug("sentry: ENV raw=%s -> normalised environment=%s",
        raw_env ? raw_env : "<unset>", env.c_str());

    const std::string dsn = cfg.store.get_str("SENTRY_DSN");
    if (dsn.empty()) {
        platform::log_warn("sentry: SENTRY_DSN is not set, not initialised");
        return false;
    }
    // DSN is a secret, so log only that it is present and its length, never the value.
    platform::log_debug("sentry: SENTRY_DSN is set (%zu chars)", dsn.size());

    const std::string db_path = cfg.store.get_str("SENTRY_DATABASE_PATH", ".sentry-native");
    const std::string release = detail::sentry_release(cfg);
    // Case-insensitive, matching platform::set_log_level (which lowercases before
    // comparing). A raw `cfg.log_level == "debug"` would leave Sentry's internal
    // logging off for LOG_LEVEL=DEBUG/Debug even though the app logger is at debug,
    // hiding the SDK's own diagnostics when sentry_init fails.
    std::string lvl = cfg.log_level;
    for (char& c : lvl) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    const int    debug_flag         = (lvl == "debug") ? 1 : 0;
    const double traces_sample_rate = 1.0;
    platform::log_debug("sentry: release resolved to %s",
        release.empty() ? "<none> (left unset, as Python returns None)" : release.c_str());

    // Crashpad backend: sentry_init launches a separate crashpad_handler process
    // and fails init ("invalid handler_path") when the path is unset — the SDK
    // does not auto-discover it. Prefer an explicit SENTRY_HANDLER_PATH, else the
    // handler that CMake copies next to this binary.
    std::string handler_path = cfg.store.get_str("SENTRY_HANDLER_PATH");
    if (handler_path.empty()) {
        const std::string dir = detail::executable_dir();
        if (!dir.empty()) {
#ifdef _WIN32
            handler_path = dir + "\\crashpad_handler.exe";
#else
            handler_path = dir + "/crashpad_handler";
#endif
        }
    }
    const bool have_handler = detail::file_exists(handler_path);

    sentry_options_t* options = sentry_options_new();
    if (!options) {
        platform::log_error("sentry: sentry_options_new() returned null; cannot initialise");
        return false;
    }
    platform::log_debug("sentry: options allocated; applying configuration");
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
        platform::log_warn(
            "sentry: crashpad_handler not found (looked for '%s'); the crashpad "
            "backend cannot start, so sentry_init will fail. Set SENTRY_HANDLER_PATH "
            "or ship crashpad_handler next to the binary.",
            handler_path.empty() ? "<undetermined>" : handler_path.c_str());
    }

    platform::log_debug(
        "sentry: options set (environment=%s, database_path=%s, traces_sample_rate=%.2f, "
        "debug=%d, release=%s, dsn=set, handler_path=%s); calling sentry_init()",
        env.c_str(), db_path.c_str(), traces_sample_rate, debug_flag,
        release.empty() ? "<auto>" : release.c_str(),
        have_handler ? handler_path.c_str() : "<none>");

    const int rc = sentry_init(options);
    platform::log_debug("sentry: sentry_init() returned %d", rc);
    if (rc != 0) {
        platform::log_error(
            "sentry: init failed (sentry_init rc=%d); continuing without crash reporting", rc);
        return false;
    }
    platform::log_info(
        "sentry: initialised (environment=%s, release=%s)",
        env.c_str(), release.empty() ? "<auto>" : release.c_str());
    return true;
}

}  // namespace mirobody

#endif  // MIROBODY_HAS_SENTRY

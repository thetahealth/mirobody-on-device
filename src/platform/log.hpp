#pragma once

// Hard compile-time floor. Catches build-system misconfiguration that
// silently downgrades the standard before the error surfaces as a missing
// std::optional deeper in. CMake can append a later `-std` flag after an
// app module's native flags, so this check protects host bridge builds.
//
// MSVC stops reporting __cplusplus correctly unless /Zc:__cplusplus is set
// (it does in this build), but consult _MSVC_LANG anyway so a stray include
// of this header from a project that forgot the flag still works.
#if defined(_MSVC_LANG)
    static_assert(_MSVC_LANG  >= 201703L, "mirobody requires C++17 or newer");
#else
    static_assert(__cplusplus >= 201703L, "mirobody requires C++17 or newer");
#endif

namespace mirobody { namespace platform {

// Minimum severity that will be emitted. Anything below the current level is
// dropped. Mirrors the LOG_LEVEL config values; the threshold is process-wide.
enum class LogLevel { Debug, Info, Warn, Error };

// Set the process-wide minimum log level. `set_log_level(const char*)` accepts
// the LOG_LEVEL spellings (case-insensitive): "debug", "info", "warning"/"warn",
// "error", "critical"/"fatal"; an unrecognized value leaves the level unchanged.
// Call once at startup, before threads spin up. Default level is Info, so
// log_debug() is silent unless the level is lowered to Debug.
void set_log_level(LogLevel level);
void set_log_level(const char* level);

void log_info(const char* fmt, ...);
void log_warn(const char* fmt, ...);
void log_error(const char* fmt, ...);
void log_debug(const char* fmt, ...);

}
}

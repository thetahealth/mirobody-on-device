#pragma once

// Wall-clock helper for database lifecycle timestamps.
//
// The DB stores created_at / updated_at / deleted_at as unix MILLISECONDS in
// BIGINT columns (uniform across SQL dialects, and the scale wearable health
// data already uses). All such columns are app-stamped via this one helper so
// the unit is consistent at every write site -- "unix" alone reads as seconds to
// most people, so the name says ms explicitly.
//
// NOTE: this is for DB row timestamps, NOT token expiry. JWT/OAuth/fernet times
// are RFC-spec SECONDS and have their own seconds-based helpers; do not use this
// for them.

#include <chrono>
#include <cstdint>

namespace mirobody { namespace platform {

inline std::int64_t now_unix_ms() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
}

}}

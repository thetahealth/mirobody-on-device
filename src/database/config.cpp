// URI builders for the per-backend config structs declared in database.hpp.
// Compiled into mirobody_core regardless of which backend is linked — the
// resulting Database object then routes through whichever backend's .cpp
// implements the constructor.

#include "database/database.hpp"

#include <cstddef>
#include <string>

namespace mirobody { namespace database {

namespace {

// True if `path` (a comma-separated PG search_path value) already names
// the `public` schema. Splits on `,`, trims whitespace and one layer of
// double quotes, then byte-equals each token to "public". Recognizes
// "public", "mirobody, public", "\"public\"" without false-matching on
// substrings like "mirobody_public". Case-sensitive: `Public` is missed,
// but PG folds the unquoted form to lowercase server-side so the worst
// outcome is a harmless extra `,public` in the conninfo.
bool search_path_has_public(const std::string& path) {
    static constexpr char        kPublic[]  = "public";
    static constexpr std::size_t kPublicLen = sizeof(kPublic) - 1;   // 6, excludes '\0'

    const std::size_t n = path.size();
    std::size_t i = 0;
    while (i <= n) {
        std::size_t end = i;
        while (end < n && path[end] != ',') ++end;

        std::size_t s = i, e = end;
        while (s < e && (path[s] == ' ' || path[s] == '\t')) ++s;
        while (e > s && (path[e - 1] == ' ' || path[e - 1] == '\t')) --e;
        if (e > s + 1 && path[s] == '"' && path[e - 1] == '"') { ++s; --e; }

        if (path.compare(s, e - s, kPublic, kPublicLen) == 0) return true;

        i = end + 1;
    }
    return false;
}

}

//------------------------------------------------------------------------------

Database PostgreSQLConfig::open() const {
    // libpq accepts a space-separated keyword=value connection string. Wrap
    // every value in single quotes and escape embedded ' / \ so passwords or
    // hostnames with special characters round-trip correctly.
    auto quote = [](const std::string& s) {
        std::string out;
        out.reserve(s.size() + 2);
        out += '\'';
        for (std::size_t i = 0; i < s.size(); ++i) {
            const char c = s[i];
            if (c == '\'' || c == '\\') out += '\\';
            out += c;
        }
        out += '\'';
        return out;
    };

    std::string uri;
    auto append = [&](const char* key, const std::string& value, bool quoted) {
        if (!uri.empty()) uri += ' ';
        uri += key;
        uri += '=';
        uri += quoted ? quote(value) : value;
    };

    if (!host.empty())     append("host",     host,     true);
    if (port > 0)          append("port",     std::to_string(port), false);
    if (!user.empty())     append("user",     user,     true);
    if (!password.empty()) append("password", password, true);
    if (!database.empty()) append("dbname",   database, true);
    if (!schema.empty()) {
        // Keep `public` in the search path so extension-installed types
        // and functions (pgvector's `vector`, uuid-ossp's `uuid_generate_v4`,
        // etc.) still resolve without explicit `public.` qualification.
        // Only append when the caller hasn't already mentioned it.
        std::string path = schema;
        if (!search_path_has_public(path)) path += ",public";
        append("options", "-c search_path=" + path, true);
    }

    // Pool sizing extensions: libpq itself does not pool, so these are
    // parsed and stripped by pg.cpp before the conninfo is handed
    // to PQconnectdb. Default sizing kicks in when they are absent.
    if (min_connection > 0) append("pool_min", std::to_string(min_connection), false);
    if (max_connection > 0) append("pool_max", std::to_string(max_connection), false);

    return Database(uri);
}

//------------------------------------------------------------------------------

Database SQLiteConfig::open() const {
    return Database(path.empty() ? std::string(":memory:") : path);
}

//------------------------------------------------------------------------------

Database MySQLConfig::open() const {
    // A space-separated keyword=value string (same shape as PostgreSQL's conninfo);
    // mysql.cpp parses it back into the connection fields. Values are single-quoted
    // with ' / \ escaped so passwords/hosts with special characters round-trip.
    auto quote = [](const std::string& s) {
        std::string out;
        out.reserve(s.size() + 2);
        out += '\'';
        for (std::size_t i = 0; i < s.size(); ++i) {
            const char c = s[i];
            if (c == '\'' || c == '\\') out += '\\';
            out += c;
        }
        out += '\'';
        return out;
    };

    std::string uri;
    auto append = [&](const char* key, const std::string& value, bool quoted) {
        if (!uri.empty()) uri += ' ';
        uri += key;
        uri += '=';
        uri += quoted ? quote(value) : value;
    };

    if (!host.empty())     append("host",     host,                  true);
    if (port > 0)          append("port",     std::to_string(port),  false);
    if (!user.empty())     append("user",     user,                  true);
    if (!password.empty()) append("password", password,              true);
    if (!database.empty()) append("dbname",   database,              true);
    if (min_connection > 0) append("pool_min", std::to_string(min_connection), false);
    if (max_connection > 0) append("pool_max", std::to_string(max_connection), false);

    return Database(uri);
}

}}

#pragma once

#include "compat/cxx11.hpp"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace mirobody { namespace cache {

class Cache;

//------------------------------------------------------------------------------
// Backend configuration
//------------------------------------------------------------------------------

// Configuration for the in-process memory backend. Carries no fields:
// the in-memory cache has nothing to tune. Defined so callers can pick a
// backend uniformly via `<config>.open()`, parallel to RedisConfig.
struct MemoryKvConfig {
    // Construct a Cache backed by a fresh MemoryKv. Equivalent to the
    // default Cache() constructor; provided for symmetry with
    // RedisConfig::open().
    Cache open() const;
};

//------------------------------------------------------------------------------

// Connection parameters for the Redis backend. Populated by load_config()
// from REDIS_HOST / REDIS_PORT / REDIS_PASSWORD / REDIS_DB / REDIS_SSL /
// REDIS_SSL_CHECK_HOSTNAME / REDIS_SSL_CERT_REQS.
struct RedisConfig {
    std::string host;
    int         port = 6379;
    std::string password;
    int         database = 0;
    bool        ssl = false;
    bool        ssl_check_hostname = false;
    std::string ssl_cert_reqs;

    // Connect to Redis with these parameters and return an owning Cache
    // backed by the connection. Throws std::runtime_error on connect /
    // TLS handshake / AUTH / SELECT failure. When `ssl=true`, TLS is used iff
    // the build linked hiredis_ssl (MIROBODY_HAS_HIREDIS_SSL); otherwise
    // `ssl=true` throws. CA trust uses OpenSSL's default verify paths and
    // `ssl_cert_reqs` selects the verify mode (none/optional/required).
    Cache open() const;
};

//------------------------------------------------------------------------------
// Cache
//------------------------------------------------------------------------------

// Thin wrapper over whichever cache backend is built into this binary.
// Mirrors database::Database: one public class, backends (in-process
// memory and hiredis-backed Redis) live behind a pimpl Impl and are
// picked at construction time.
//
// API surface is taken from cache::MemoryKv so the two are interchangeable:
// callers that need a private cache instantiate Cache directly, callers
// that need raw in-memory access can still use MemoryKv. Time points use
// std::chrono::steady_clock; the Redis backend converts to PX (millisecond
// TTL) under the hood, which is monotonic-safe.
//
// Not thread-safe. The in-memory backend serializes via std::unordered_map
// semantics; the Redis backend serializes I/O on a single hiredis
// connection. Open one Cache per worker or wrap with a mutex.
class Cache {
public:
    using clock = std::chrono::steady_clock;

    // Backend interface. Defined in cache.cpp so its derivatives can live
    // in the .cpp's anonymous namespace; declared here so unique_ptr<Impl>
    // can name it. Consumers cannot subclass meaningfully (the definition
    // is hidden).
    struct Impl;

    // Default constructor: an isolated in-process MemoryKv backend.
    Cache();

    // Open a hiredis-backed Cache using `cfg`. Equivalent to cfg.open().
    explicit Cache(const RedisConfig& cfg);

    ~Cache();

    Cache(const Cache&) = delete;
    Cache& operator=(const Cache&) = delete;
    Cache(Cache&&) noexcept;
    Cache& operator=(Cache&&) noexcept;

    // Insert or replace `key` with `value`, expiring at the given time
    // point. Pass clock::time_point::max() for a never-expiring entry.
    // On the Redis backend, an `expires_at <= now()` collapses to DEL.
    void set(std::string key, std::string value, clock::time_point expires_at);

    // Convenience: expires_at = clock::now() + ttl.
    void set(std::string key, std::string value, clock::duration ttl);

    // GET. Returns the value, or mirobody::nullopt when the key is absent
    // or expired.
    mirobody::optional<std::string> get(const std::string& key);

    // INCR / DECR. Returns the new integer value, or mirobody::nullopt
    // when the existing value does not parse as a signed 64-bit integer
    // or when the operation would overflow. A missing key is created at
    // 0 first (resulting value 1 / -1) with no expiration.
    mirobody::optional<std::int64_t> incr(const std::string& key);
    mirobody::optional<std::int64_t> decr(const std::string& key);

    // RPUSH. Appends to the list at `key`, creating it (with no expiration)
    // when absent. Returns the new list length, or 0 when the key holds a
    // string (WRONGTYPE) or `values` is empty. Note that get() on a list
    // key returns nullopt and set() replaces the whole list, as in Redis.
    std::size_t rpush(const std::string& key, const std::string& value);
    std::size_t rpush(const std::string& key, const std::vector<std::string>& values);

    // LPUSH. Same contract as rpush() but prepends, one value at a time in
    // argument order -- lpush("k", {"a", "b"}) leaves "b" first, as in Redis.
    std::size_t lpush(const std::string& key, const std::string& value);
    std::size_t lpush(const std::string& key, const std::vector<std::string>& values);

    // LPOP / RPOP (single-element form). Removes and returns the first /
    // last list element; nullopt when the key is absent or holds a string
    // (WRONGTYPE). Popping the last element removes the key.
    mirobody::optional<std::string> lpop(const std::string& key);
    mirobody::optional<std::string> rpop(const std::string& key);

    // LRANGE. The list elements at [start, stop], inclusive; negative
    // indices count from the end (-1 is the last element), out-of-range
    // indices are clamped. Empty when the key is absent / a string or the
    // range is empty.
    std::vector<std::string> lrange(const std::string& key,
                                    std::int64_t start, std::int64_t stop);

    // LTRIM. Keeps only the list elements at [start, stop] (same index
    // rules as lrange). Trimming to an empty range removes the key. No-op
    // on a string key.
    void ltrim(const std::string& key, std::int64_t start, std::int64_t stop);

    // ATOMICALLY render the list at `list` into a string and store it at
    // `dest` with `ttl`: dest = prefix + join(rows, sep) + suffix. Returns
    // the stored string, or nullopt -- with dest untouched -- when `list`
    // is absent, empty, or not a list. The read and the write are one
    // step (a Lua EVAL on the Redis backend, the store mutex in-memory),
    // so a rendering computed from an older list state can never overwrite
    // one computed from a newer state. No Redis command equivalent.
    mirobody::optional<std::string> set_join(const std::string& dest,
                                             const std::string& list,
                                             const std::string& prefix,
                                             const std::string& sep,
                                             const std::string& suffix,
                                             clock::duration ttl);

    // PEXPIRE / PEXPIREAT / PERSIST. Re-anchors the expiration of an
    // existing key (string or list) without touching its value;
    // clock::time_point::max() clears the TTL, a deadline at or before
    // now() expires the key immediately. Returns true when an existing
    // key's expiration was updated.
    bool expire(const std::string& key, clock::time_point expires_at);
    bool expire(const std::string& key, clock::duration ttl);

    // EXISTS (single-key form).
    bool exists(const std::string& key);

    // DEL (single-key form). Returns true if the key had a mapping.
    bool del(const std::string& key);

    // Absolute expiration for `key`, or mirobody::nullopt when the key
    // is absent / expired. clock::time_point::max() signals "no TTL".
    // The Redis backend translates from PTTL.
    mirobody::optional<clock::time_point> expiretime(const std::string& key);

    // Evict expired entries up-front. In-memory: scans and erases; returns
    // the number removed. Redis: always returns 0 because the server
    // handles eviction internally.
    std::size_t prune();

    // FLUSHDB.
    void flushdb();

    // DBSIZE. For the Redis backend this is an RTT.
    std::size_t dbsize();

    // True iff dbsize() == 0.
    bool empty();

private:
    std::unique_ptr<Impl> impl_;
};

}
}

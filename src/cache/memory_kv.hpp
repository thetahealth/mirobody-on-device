#pragma once

#include "compat/cxx11.hpp"

#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

namespace mirobody { namespace cache {

// Process-local key/value cache with per-entry expiration. Keys and values
// are both std::string; expirations are anchored to a monotonic
// std::chrono::steady_clock so wall-clock adjustments cannot resurrect or
// prematurely evict entries.
//
// Method names follow Redis vocabulary (set / get / rpush / lpush /
// lpop / rpop / lrange / ltrim / exists / del / expire / expiretime /
// dbsize / flushdb) so the surface area carries over to the
// hiredis-backed cache::Cache implementation. An entry is either a
// string or a list, as in Redis: string operations on a list key (and
// vice versa) fail the way Redis WRONGTYPE does -- get/incr/lpop return
// nullopt, rpush returns 0, lrange returns empty -- while the
// type-agnostic operations (exists / del / expire / expiretime) work on
// both.
//
// Eviction is both lazy and proactive: expired entries are evicted on the
// next lookup (get / exists / expiretime), and a background sweeper
// thread calls prune() once per second so memory does not grow unbounded
// with stale TTL keys the workload never reads back. The sweep interval
// is fixed at one second; the thread runs for the lifetime of the
// MemoryKv instance.
//
// Thread-safe: every public method serializes on an internal mutex, and
// the sweeper acquires the same mutex for its pass. Move and copy are
// deleted because the sweeper thread captures `this`.
class MemoryKv {
public:
    using clock = std::chrono::steady_clock;

    MemoryKv();
    ~MemoryKv();

    MemoryKv(const MemoryKv&)            = delete;
    MemoryKv& operator=(const MemoryKv&) = delete;
    MemoryKv(MemoryKv&&)                 = delete;
    MemoryKv& operator=(MemoryKv&&)      = delete;

    // Returns the process-wide MemoryKv singleton. The instance is the
    // SAME for every caller in the program; construct your own MemoryKv
    // directly if you need an isolated cache (e.g. for tests). First
    // call lazily initializes the singleton with C++11 magic-static
    // semantics. The singleton's sweeper runs for the program lifetime
    // and is joined on static-destruction at exit.
    static MemoryKv& open();

    // Insert or replace `key` with `value`, expiring at the given time
    // point. Pass clock::time_point::max() for entries that should never
    // expire.
    void set(std::string key, std::string value, clock::time_point expires_at);

    // Convenience overload: expires_at = clock::now() + ttl. A ttl of
    // clock::duration::zero() or negative makes the entry already
    // expired (effectively a no-op once the next lookup or sweep runs).
    void set(std::string key, std::string value, clock::duration ttl);

    // Returns the value if `key` is present and unexpired. Lazily evicts
    // the entry when it is found expired. Mirrors Redis GET.
    mirobody::optional<std::string> get(const std::string& key);

    // Atomically adds 1 to the integer stored at `key` and returns the
    // new value. A missing or expired key is created at 0 first
    // (resulting value 1). Returns mirobody::nullopt when the existing
    // value does not parse as a signed 64-bit integer or when the
    // increment would overflow. An existing key's expiration is
    // preserved; a newly-created key has no expiration. Mirrors Redis
    // INCR.
    mirobody::optional<std::int64_t> incr(const std::string& key);

    // Same contract as incr() but subtracts 1. Mirrors Redis DECR.
    mirobody::optional<std::int64_t> decr(const std::string& key);

    // Append `values` to the list at `key`, creating it (with no
    // expiration) when absent. Returns the new list length, or 0 when
    // `key` holds a string (WRONGTYPE) or `values` is empty. Mirrors
    // Redis RPUSH.
    std::size_t rpush(const std::string& key, const std::vector<std::string>& values);

    // Same contract as rpush() but prepends, one value at a time in
    // argument order -- lpush("k", {"a", "b"}) leaves "b" first, as in
    // Redis. Mirrors Redis LPUSH.
    std::size_t lpush(const std::string& key, const std::vector<std::string>& values);

    // Remove and return the first / last list element. nullopt when the
    // key is absent / expired or holds a string (WRONGTYPE). Popping the
    // last element removes the key, as in Redis. Mirror Redis LPOP / RPOP
    // (single-element form).
    mirobody::optional<std::string> lpop(const std::string& key);
    mirobody::optional<std::string> rpop(const std::string& key);

    // The list elements at [start, stop], inclusive; negative indices
    // count from the end (-1 is the last element), out-of-range indices
    // are clamped. Empty when the key is absent / expired / a string or
    // the range is empty. Mirrors Redis LRANGE.
    std::vector<std::string> lrange(const std::string& key,
                                    std::int64_t start, std::int64_t stop);

    // Keep only the list elements at [start, stop] (same index rules as
    // lrange). Trimming to an empty range removes the key, as in Redis.
    // No-op on a string key. Mirrors Redis LTRIM.
    void ltrim(const std::string& key, std::int64_t start, std::int64_t stop);

    // Atomically (under the store mutex) render the list at `list` into a
    // string -- prefix + join(rows, sep) + suffix -- and store it at `dest`
    // with `ttl`, returning the stored string. nullopt, with dest
    // untouched, when `list` is absent / expired / empty / a string. No
    // Redis command equivalent; the hiredis backend runs it as a Lua
    // script.
    mirobody::optional<std::string> set_join(const std::string& dest,
                                             const std::string& list,
                                             const std::string& prefix,
                                             const std::string& sep,
                                             const std::string& suffix,
                                             clock::duration ttl);

    // Re-anchor the expiration of an existing key (string or list) without
    // touching its value; clock::time_point::max() clears the TTL. Returns
    // true when the key was present and unexpired (and lazily evicts it
    // otherwise). Mirrors Redis PEXPIREAT / PERSIST.
    bool expire(const std::string& key, clock::time_point expires_at);

    // Convenience overload: expires_at = clock::now() + ttl. Mirrors
    // Redis PEXPIRE.
    bool expire(const std::string& key, clock::duration ttl);

    // True if `key` is present and unexpired. Lazily evicts on expiration.
    // Mirrors Redis EXISTS (single-key form).
    bool exists(const std::string& key);

    // Returns the expiration time for `key` if it is present and unexpired,
    // otherwise mirobody::nullopt. Lazily evicts on expiration. Mirrors
    // Redis EXPIRETIME, though the returned time point is on steady_clock
    // rather than a Unix timestamp.
    mirobody::optional<clock::time_point> expiretime(const std::string& key);

    // Remove a single key. Returns true if a mapping existed (regardless
    // of whether it had already expired). Mirrors Redis DEL (single-key
    // form), where the return is the count actually removed.
    bool del(const std::string& key);

    // Evict every entry whose expiration is at or before clock::now() and
    // return the number removed. Called once per second by the background
    // sweeper; callers can also invoke it directly to force an immediate
    // pass.
    std::size_t prune();

    // Drop all entries. Mirrors Redis FLUSHDB.
    void flushdb();

    // Raw entry count, including expired-but-not-yet-pruned entries.
    // Mirrors Redis DBSIZE.
    std::size_t dbsize() const;

    // True if no entries (expired or not) are stored. No direct Redis
    // equivalent; kept as a cheap stdlib-style convenience.
    bool empty() const;

private:
    struct Entry {
        bool                     is_list = false;
        std::string              value;   // string entry (is_list == false)
        std::vector<std::string> list;    // list entry   (is_list == true)
        clock::time_point        expires_at;
    };

    // entries_.find() that treats an expired entry as absent, evicting it
    // lazily -- the lookup step every operation starts with.
    std::unordered_map<std::string, Entry>::iterator find_live_locked(const std::string& key);

    mirobody::optional<std::int64_t> apply_delta_locked(const std::string& key, std::int64_t delta);
    mirobody::optional<std::string>  pop_locked(const std::string& key, bool front);
    std::size_t prune_locked();
    void        sweep_loop();

    mutable std::mutex                     mu_;
    std::condition_variable                cv_;
    bool                                   stopping_ = false;
    std::unordered_map<std::string, Entry> entries_;
    std::thread                            sweeper_;
};

}
}

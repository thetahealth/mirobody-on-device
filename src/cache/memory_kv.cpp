#include "cache/memory_kv.hpp"
#include <optional>

#include <limits>
#include <utility>

namespace mirobody { namespace cache {

//------------------------------------------------------------------------------

MemoryKv& MemoryKv::open() {
    static MemoryKv instance;
    return instance;
}

namespace {

//------------------------------------------------------------------------------

// Parse `s` as a signed 64-bit integer with no leading/trailing characters.
// Matches Redis INCR's parsing strictness: "1" parses, " 1" / "1 " / "1.0"
// / empty all fail.
std::optional<std::int64_t> parse_i64(const std::string& s) {
    if (s.empty()) return std::nullopt;
    try {
        std::size_t pos = 0;
        long long v = std::stoll(s, &pos);
        if (pos != s.size()) return std::nullopt;
        return static_cast<std::int64_t>(v);
    } catch (...) {
        return std::nullopt;
    }
}

// Normalize a Redis-style inclusive [start, stop] range (negative indices
// count from the end) against a list of `len` elements, clamping to bounds.
// False when the resulting range is empty.
bool normalize_range(std::int64_t len, std::int64_t& start, std::int64_t& stop) {
    if (start < 0) start += len;
    if (stop  < 0) stop  += len;
    if (start < 0) start = 0;
    if (start >= len || stop < start) return false;
    if (stop >= len) stop = len - 1;
    return true;
}

}

//------------------------------------------------------------------------------

MemoryKv::MemoryKv() : sweeper_(&MemoryKv::sweep_loop, this) {}

//------------------------------------------------------------------------------

MemoryKv::~MemoryKv() {
    {
        std::lock_guard<std::mutex> lock(mu_);
        stopping_ = true;
    }
    cv_.notify_all();
    if (sweeper_.joinable()) sweeper_.join();
}

//------------------------------------------------------------------------------

std::unordered_map<std::string, MemoryKv::Entry>::iterator
MemoryKv::find_live_locked(const std::string& key) {
    auto it = entries_.find(key);
    if (it != entries_.end() && it->second.expires_at <= clock::now()) {
        entries_.erase(it);
        it = entries_.end();
    }
    return it;
}

//------------------------------------------------------------------------------

void MemoryKv::set(std::string key, std::string value, clock::time_point expires_at) {
    std::lock_guard<std::mutex> lock(mu_);
    Entry& e = entries_[std::move(key)];
    // SET replaces an existing list with a string, as in Redis.
    e.is_list = false;
    e.list.clear();
    e.value      = std::move(value);
    e.expires_at = expires_at;
}

//------------------------------------------------------------------------------

void MemoryKv::set(std::string key, std::string value, clock::duration ttl) {
    set(std::move(key), std::move(value), clock::now() + ttl);
}

//------------------------------------------------------------------------------

std::optional<std::string> MemoryKv::get(const std::string& key) {
    std::lock_guard<std::mutex> lock(mu_);
    auto it = find_live_locked(key);
    if (it == entries_.end() || it->second.is_list) return std::nullopt;
    return it->second.value;
}

//------------------------------------------------------------------------------

std::optional<std::int64_t> MemoryKv::incr(const std::string& key) {
    std::lock_guard<std::mutex> lock(mu_);
    return apply_delta_locked(key, 1);
}

//------------------------------------------------------------------------------

std::optional<std::int64_t> MemoryKv::decr(const std::string& key) {
    std::lock_guard<std::mutex> lock(mu_);
    return apply_delta_locked(key, -1);
}

//------------------------------------------------------------------------------

std::optional<std::int64_t>
MemoryKv::apply_delta_locked(const std::string& key, std::int64_t delta) {
    auto it = find_live_locked(key);
    if (it != entries_.end() && it->second.is_list) return std::nullopt;

    std::int64_t current = 0;
    if (it != entries_.end()) {
        auto parsed = parse_i64(it->second.value);
        if (!parsed) return std::nullopt;
        current = *parsed;
    }

    // Manual overflow guard (no portable __builtin_add_overflow on MSVC).
    constexpr auto kMax = std::numeric_limits<std::int64_t>::max();
    constexpr auto kMin = std::numeric_limits<std::int64_t>::min();
    if (delta > 0 && current > kMax - delta) return std::nullopt;
    if (delta < 0 && current < kMin - delta) return std::nullopt;

    const std::int64_t next = current + delta;

    if (it != entries_.end()) {
        it->second.value = std::to_string(next);
    } else {
        Entry& e = entries_[key];
        e.value      = std::to_string(next);
        e.expires_at = clock::time_point::max();
    }
    return next;
}

//------------------------------------------------------------------------------

std::size_t MemoryKv::rpush(const std::string& key,
                            const std::vector<std::string>& values) {
    if (values.empty()) return 0;
    std::lock_guard<std::mutex> lock(mu_);
    auto it = find_live_locked(key);
    if (it == entries_.end()) {
        Entry& e = entries_[key];
        e.is_list    = true;
        e.list       = values;
        e.expires_at = clock::time_point::max();
        return e.list.size();
    }
    if (!it->second.is_list) return 0;   // WRONGTYPE
    it->second.list.insert(it->second.list.end(), values.begin(), values.end());
    return it->second.list.size();
}

//------------------------------------------------------------------------------

std::size_t MemoryKv::lpush(const std::string& key,
                            const std::vector<std::string>& values) {
    if (values.empty()) return 0;
    std::lock_guard<std::mutex> lock(mu_);
    auto it = find_live_locked(key);
    if (it == entries_.end()) {
        Entry& e = entries_[key];
        e.is_list = true;
        // One prepend per value, as in Redis: the last value ends up first.
        e.list.assign(values.rbegin(), values.rend());
        e.expires_at = clock::time_point::max();
        return e.list.size();
    }
    if (!it->second.is_list) return 0;   // WRONGTYPE
    it->second.list.insert(it->second.list.begin(), values.rbegin(), values.rend());
    return it->second.list.size();
}

//------------------------------------------------------------------------------

std::optional<std::string> MemoryKv::lpop(const std::string& key) {
    std::lock_guard<std::mutex> lock(mu_);
    return pop_locked(key, true);
}

//------------------------------------------------------------------------------

std::optional<std::string> MemoryKv::rpop(const std::string& key) {
    std::lock_guard<std::mutex> lock(mu_);
    return pop_locked(key, false);
}

//------------------------------------------------------------------------------

std::optional<std::string>
MemoryKv::pop_locked(const std::string& key, bool front) {
    auto it = find_live_locked(key);
    if (it == entries_.end() || !it->second.is_list) return std::nullopt;
    std::vector<std::string>& list = it->second.list;
    std::string out = std::move(front ? list.front() : list.back());
    if (front) {
        list.erase(list.begin());
    } else {
        list.pop_back();
    }
    // An emptied list's key goes away, as in Redis.
    if (list.empty()) entries_.erase(it);
    return out;
}

//------------------------------------------------------------------------------

std::vector<std::string> MemoryKv::lrange(const std::string& key,
                                          std::int64_t start, std::int64_t stop) {
    std::lock_guard<std::mutex> lock(mu_);
    auto it = find_live_locked(key);
    if (it == entries_.end() || !it->second.is_list) return std::vector<std::string>();
    const std::vector<std::string>& list = it->second.list;
    if (!normalize_range(static_cast<std::int64_t>(list.size()), start, stop)) {
        return std::vector<std::string>();
    }
    return std::vector<std::string>(list.begin() + static_cast<std::ptrdiff_t>(start),
                                    list.begin() + static_cast<std::ptrdiff_t>(stop) + 1);
}

//------------------------------------------------------------------------------

void MemoryKv::ltrim(const std::string& key,
                     std::int64_t start, std::int64_t stop) {
    std::lock_guard<std::mutex> lock(mu_);
    auto it = find_live_locked(key);
    if (it == entries_.end() || !it->second.is_list) return;
    std::vector<std::string>& list = it->second.list;
    if (!normalize_range(static_cast<std::int64_t>(list.size()), start, stop)) {
        entries_.erase(it);   // trimmed to nothing: the key goes away, as in Redis
        return;
    }
    list.erase(list.begin() + static_cast<std::ptrdiff_t>(stop) + 1, list.end());
    list.erase(list.begin(), list.begin() + static_cast<std::ptrdiff_t>(start));
}

//------------------------------------------------------------------------------

std::optional<std::string> MemoryKv::set_join(const std::string& dest,
                                                   const std::string& list,
                                                   const std::string& prefix,
                                                   const std::string& sep,
                                                   const std::string& suffix,
                                                   clock::duration ttl) {
    std::lock_guard<std::mutex> lock(mu_);
    auto it = find_live_locked(list);
    if (it == entries_.end() || !it->second.is_list || it->second.list.empty()) {
        return std::nullopt;
    }

    // Assemble before touching entries_: inserting dest may rehash the map
    // and invalidate `it` (and dest may even BE the list key -- the write
    // below then replaces it, as Redis SET would).
    std::string out = prefix;
    const std::vector<std::string>& rows = it->second.list;
    for (std::size_t i = 0; i < rows.size(); ++i) {
        if (i) out += sep;
        out += rows[i];
    }
    out += suffix;

    Entry& e = entries_[dest];
    e.is_list = false;
    e.list.clear();
    e.value      = out;
    e.expires_at = clock::now() + ttl;
    return out;
}

//------------------------------------------------------------------------------

bool MemoryKv::expire(const std::string& key, clock::time_point expires_at) {
    std::lock_guard<std::mutex> lock(mu_);
    auto it = find_live_locked(key);
    if (it == entries_.end()) return false;
    // Clearing the TTL of a key that has none reports false, as Redis
    // PERSIST does (so the two Cache backends stay interchangeable).
    if (expires_at == clock::time_point::max() &&
        it->second.expires_at == clock::time_point::max()) {
        return false;
    }
    it->second.expires_at = expires_at;
    return true;
}

//------------------------------------------------------------------------------

bool MemoryKv::expire(const std::string& key, clock::duration ttl) {
    return expire(key, clock::now() + ttl);
}

//------------------------------------------------------------------------------

bool MemoryKv::exists(const std::string& key) {
    std::lock_guard<std::mutex> lock(mu_);
    auto it = entries_.find(key);
    if (it == entries_.end()) return false;
    if (it->second.expires_at <= clock::now()) {
        entries_.erase(it);
        return false;
    }
    return true;
}

//------------------------------------------------------------------------------

std::optional<MemoryKv::clock::time_point>
MemoryKv::expiretime(const std::string& key) {
    std::lock_guard<std::mutex> lock(mu_);
    auto it = entries_.find(key);
    if (it == entries_.end()) return std::nullopt;
    if (it->second.expires_at <= clock::now()) {
        entries_.erase(it);
        return std::nullopt;
    }
    return it->second.expires_at;
}

//------------------------------------------------------------------------------

bool MemoryKv::del(const std::string& key) {
    std::lock_guard<std::mutex> lock(mu_);
    return entries_.erase(key) > 0;
}

//------------------------------------------------------------------------------

std::size_t MemoryKv::prune() {
    std::lock_guard<std::mutex> lock(mu_);
    return prune_locked();
}

//------------------------------------------------------------------------------

std::size_t MemoryKv::prune_locked() {
    const auto now = clock::now();
    std::size_t evicted = 0;
    for (auto it = entries_.begin(); it != entries_.end(); ) {
        if (it->second.expires_at <= now) {
            it = entries_.erase(it);
            ++evicted;
        } else {
            ++it;
        }
    }
    return evicted;
}

//------------------------------------------------------------------------------

void MemoryKv::flushdb() {
    std::lock_guard<std::mutex> lock(mu_);
    entries_.clear();
}

//------------------------------------------------------------------------------

std::size_t MemoryKv::dbsize() const {
    std::lock_guard<std::mutex> lock(mu_);
    return entries_.size();
}

//------------------------------------------------------------------------------

bool MemoryKv::empty() const {
    std::lock_guard<std::mutex> lock(mu_);
    return entries_.empty();
}

//------------------------------------------------------------------------------

void MemoryKv::sweep_loop() {
    std::unique_lock<std::mutex> lock(mu_);
    while (true) {
        // wait_for releases the lock while waiting and reacquires on
        // return. Wakes early when the destructor sets stopping_ and
        // notifies; otherwise wakes once per second to sweep.
        cv_.wait_for(lock, std::chrono::seconds(1), [this]{ return stopping_; });
        if (stopping_) return;
        (void)prune_locked();
    }
}

}
}

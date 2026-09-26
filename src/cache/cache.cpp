#include "cache/cache.hpp"

#include "cache/memory_kv.hpp"


#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <stdexcept>
#include <string>
#include <utility>

namespace mirobody { namespace cache {

//------------------------------------------------------------------------------
// Backend interface
//------------------------------------------------------------------------------

struct Cache::Impl {
    virtual ~Impl() = default;

    virtual void set_at (std::string key, std::string value, Cache::clock::time_point expires_at) = 0;
    virtual void set_ttl(std::string key, std::string value, Cache::clock::duration ttl)         = 0;

    virtual mirobody::optional<std::string>  get (const std::string& key) = 0;
    virtual mirobody::optional<std::int64_t> incr(const std::string& key) = 0;
    virtual mirobody::optional<std::int64_t> decr(const std::string& key) = 0;

    virtual std::size_t rpush(const std::string& key, const std::vector<std::string>& values) = 0;
    virtual std::size_t lpush(const std::string& key, const std::vector<std::string>& values) = 0;
    virtual mirobody::optional<std::string> lpop(const std::string& key) = 0;
    virtual mirobody::optional<std::string> rpop(const std::string& key) = 0;
    virtual std::vector<std::string> lrange(const std::string& key, std::int64_t start, std::int64_t stop) = 0;
    virtual void ltrim(const std::string& key, std::int64_t start, std::int64_t stop) = 0;
    virtual mirobody::optional<std::string> set_join(const std::string& dest, const std::string& list,
                                                     const std::string& prefix, const std::string& sep,
                                                     const std::string& suffix, Cache::clock::duration ttl) = 0;

    virtual bool exists   (const std::string& key) = 0;
    virtual bool del      (const std::string& key) = 0;
    virtual bool expire_at(const std::string& key, Cache::clock::time_point expires_at) = 0;

    virtual mirobody::optional<Cache::clock::time_point> expiretime(const std::string& key) = 0;

    virtual std::size_t prune()   = 0;
    virtual void        flushdb() = 0;
    virtual std::size_t dbsize()  = 0;
};

namespace {

//------------------------------------------------------------------------------
// In-process backend: forwards to an owned MemoryKv.
//------------------------------------------------------------------------------

class MemoryImpl : public Cache::Impl {
public:
    void set_at(std::string key, std::string value, Cache::clock::time_point expires_at) override {
        kv_.set(std::move(key), std::move(value), expires_at);
    }
    void set_ttl(std::string key, std::string value, Cache::clock::duration ttl) override {
        kv_.set(std::move(key), std::move(value), ttl);
    }
    mirobody::optional<std::string>  get (const std::string& key) override { return kv_.get(key); }
    mirobody::optional<std::int64_t> incr(const std::string& key) override { return kv_.incr(key); }
    mirobody::optional<std::int64_t> decr(const std::string& key) override { return kv_.decr(key); }
    std::size_t rpush(const std::string& key, const std::vector<std::string>& values) override {
        return kv_.rpush(key, values);
    }
    std::size_t lpush(const std::string& key, const std::vector<std::string>& values) override {
        return kv_.lpush(key, values);
    }
    mirobody::optional<std::string> lpop(const std::string& key) override { return kv_.lpop(key); }
    mirobody::optional<std::string> rpop(const std::string& key) override { return kv_.rpop(key); }
    std::vector<std::string> lrange(const std::string& key, std::int64_t start, std::int64_t stop) override {
        return kv_.lrange(key, start, stop);
    }
    void ltrim(const std::string& key, std::int64_t start, std::int64_t stop) override {
        kv_.ltrim(key, start, stop);
    }
    mirobody::optional<std::string> set_join(const std::string& dest, const std::string& list,
                                             const std::string& prefix, const std::string& sep,
                                             const std::string& suffix, Cache::clock::duration ttl) override {
        return kv_.set_join(dest, list, prefix, sep, suffix, ttl);
    }
    bool exists(const std::string& key) override { return kv_.exists(key); }
    bool del   (const std::string& key) override { return kv_.del(key); }
    bool expire_at(const std::string& key, Cache::clock::time_point expires_at) override {
        return kv_.expire(key, expires_at);
    }
    mirobody::optional<Cache::clock::time_point> expiretime(const std::string& key) override {
        return kv_.expiretime(key);
    }
    std::size_t prune()   override { return kv_.prune(); }
    void        flushdb() override { kv_.flushdb(); }
    std::size_t dbsize()  override { return kv_.dbsize(); }

private:
    MemoryKv kv_;
};

}

//------------------------------------------------------------------------------
// Cache front
//------------------------------------------------------------------------------

Cache MemoryKvConfig::open() const {
    return Cache{};
}

//------------------------------------------------------------------------------

Cache::Cache() : impl_(new MemoryImpl()) {}

//------------------------------------------------------------------------------

Cache::~Cache() = default;
Cache::Cache(Cache&&) noexcept = default;
Cache& Cache::operator=(Cache&&) noexcept = default;

//------------------------------------------------------------------------------

void Cache::set(std::string key, std::string value, clock::time_point expires_at) {
    impl_->set_at(std::move(key), std::move(value), expires_at);
}

//------------------------------------------------------------------------------

void Cache::set(std::string key, std::string value, clock::duration ttl) {
    impl_->set_ttl(std::move(key), std::move(value), ttl);
}

//------------------------------------------------------------------------------

mirobody::optional<std::string>  Cache::get (const std::string& key) { return impl_->get(key); }
mirobody::optional<std::int64_t> Cache::incr(const std::string& key) { return impl_->incr(key); }
mirobody::optional<std::int64_t> Cache::decr(const std::string& key) { return impl_->decr(key); }

std::size_t Cache::rpush(const std::string& key, const std::string& value) {
    return impl_->rpush(key, std::vector<std::string>(1, value));
}
std::size_t Cache::rpush(const std::string& key, const std::vector<std::string>& values) {
    return impl_->rpush(key, values);
}
std::size_t Cache::lpush(const std::string& key, const std::string& value) {
    return impl_->lpush(key, std::vector<std::string>(1, value));
}
std::size_t Cache::lpush(const std::string& key, const std::vector<std::string>& values) {
    return impl_->lpush(key, values);
}
mirobody::optional<std::string> Cache::lpop(const std::string& key) { return impl_->lpop(key); }
mirobody::optional<std::string> Cache::rpop(const std::string& key) { return impl_->rpop(key); }
std::vector<std::string> Cache::lrange(const std::string& key,
                                       std::int64_t start, std::int64_t stop) {
    return impl_->lrange(key, start, stop);
}
void Cache::ltrim(const std::string& key, std::int64_t start, std::int64_t stop) {
    impl_->ltrim(key, start, stop);
}
mirobody::optional<std::string> Cache::set_join(const std::string& dest,
                                                const std::string& list,
                                                const std::string& prefix,
                                                const std::string& sep,
                                                const std::string& suffix,
                                                clock::duration ttl) {
    return impl_->set_join(dest, list, prefix, sep, suffix, ttl);
}

bool Cache::exists(const std::string& key) { return impl_->exists(key); }
bool Cache::del   (const std::string& key) { return impl_->del(key); }

bool Cache::expire(const std::string& key, clock::time_point expires_at) {
    return impl_->expire_at(key, expires_at);
}
bool Cache::expire(const std::string& key, clock::duration ttl) {
    return impl_->expire_at(key, clock::now() + ttl);
}

mirobody::optional<Cache::clock::time_point>
Cache::expiretime(const std::string& key) { return impl_->expiretime(key); }

std::size_t Cache::prune()   { return impl_->prune(); }
void        Cache::flushdb() { impl_->flushdb(); }
std::size_t Cache::dbsize()  { return impl_->dbsize(); }
bool        Cache::empty()   { return dbsize() == 0; }

}
}

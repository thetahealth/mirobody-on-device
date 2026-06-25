#include "cache/cache.hpp"

#include "cache/memory_kv.hpp"

#include <hiredis/hiredis.h>
#if defined(MIROBODY_HAS_HIREDIS_SSL)
#  include <hiredis/hiredis_ssl.h>
#endif

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

//------------------------------------------------------------------------------
// Redis backend helpers
//------------------------------------------------------------------------------

// RAII for a redisReply returned by redisCommand / redisGetReply.
struct Reply {
    redisReply* r = nullptr;

    Reply() = default;
    explicit Reply(void* p) : r(static_cast<redisReply*>(p)) {}
    ~Reply() { if (r) freeReplyObject(r); }

    Reply(const Reply&) = delete;
    Reply& operator=(const Reply&) = delete;

    Reply(Reply&& o) noexcept : r(o.r) { o.r = nullptr; }
    Reply& operator=(Reply&& o) noexcept {
        if (this != &o) {
            if (r) freeReplyObject(r);
            r   = o.r;
            o.r = nullptr;
        }
        return *this;
    }
};

// Throw on a failed connection-setup command (AUTH / SELECT). Takes
// ownership of the raw reply pointer and frees it in all paths. Caller
// is responsible for freeing the context after the throw propagates.
void check_setup_reply(redisContext* ctx, const char* op, void* raw_reply) {
    Reply reply{raw_reply};
    if (!reply.r) {
        throw std::runtime_error(std::string{"cache::Cache: "} + op + " failed: " + ctx->errstr);
    }
    if (reply.r->type == REDIS_REPLY_ERROR) {
        std::string detail(reply.r->str, static_cast<std::size_t>(reply.r->len));
        throw std::runtime_error(std::string{"cache::Cache: "} + op + " failed: " + detail);
    }
}

//------------------------------------------------------------------------------
// Redis backend: hiredis-backed connection.
//------------------------------------------------------------------------------

class RedisImpl : public Cache::Impl {
public:
    explicit RedisImpl(const RedisConfig& cfg) {
        if (cfg.host.empty()) {
            throw std::runtime_error("cache::Cache: empty host");
        }

#if defined(MIROBODY_HAS_HIREDIS_SSL)
        if (cfg.ssl) {
            ssl_ctx_ = make_ssl_context(cfg);   // throws on failure
        }
#else
        if (cfg.ssl) {
            throw std::runtime_error(
                "cache::Cache: REDIS_SSL=true but this build has no hiredis TLS "
                "support (rebuild with libhiredis_ssl available)");
        }
#endif

        // Throwing from here on must release ssl_ctx_ explicitly: the destructor
        // does not run for a constructor that throws, so it can't clean it up.
        ctx_ = redisConnect(cfg.host.c_str(), cfg.port);
        if (!ctx_) {
            free_ssl_context();
            throw std::runtime_error("cache::Cache: redisConnect returned null");
        }
        if (ctx_->err) {
            std::string msg = std::string{"cache::Cache: connect: "} + ctx_->errstr;
            redisFree(ctx_);
            ctx_ = nullptr;
            free_ssl_context();
            throw std::runtime_error(msg);
        }

#if defined(MIROBODY_HAS_HIREDIS_SSL)
        // Upgrade the established TCP connection to TLS before any AUTH/SELECT so
        // credentials never traverse the wire in clear text.
        if (cfg.ssl && redisInitiateSSLWithContext(ctx_, ssl_ctx_) != REDIS_OK) {
            std::string msg = std::string{"cache::Cache: TLS handshake: "} + ctx_->errstr;
            redisFree(ctx_);
            ctx_ = nullptr;
            free_ssl_context();
            throw std::runtime_error(msg);
        }
#endif

        try {
            if (!cfg.password.empty()) {
                check_setup_reply(ctx_, "AUTH",
                                  redisCommand(ctx_, "AUTH %s", cfg.password.c_str()));
            }
            if (cfg.database != 0) {
                check_setup_reply(ctx_, "SELECT",
                                  redisCommand(ctx_, "SELECT %d", cfg.database));
            }
        } catch (...) {
            redisFree(ctx_);
            ctx_ = nullptr;
            free_ssl_context();
            throw;
        }
    }

    ~RedisImpl() override {
        if (ctx_) redisFree(ctx_);
        free_ssl_context();
    }

    void set_at(std::string key, std::string value, Cache::clock::time_point expires_at) override {
        if (expires_at == Cache::clock::time_point::max()) {
            Reply r{redisCommand(ctx_, "SET %b %b",
                                 key.data(),   static_cast<std::size_t>(key.size()),
                                 value.data(), static_cast<std::size_t>(value.size()))};
            (void)r;
            return;
        }
        const auto now = Cache::clock::now();
        if (expires_at <= now) {
            Reply r{redisCommand(ctx_, "DEL %b",
                                 key.data(), static_cast<std::size_t>(key.size()))};
            (void)r;
            return;
        }
        const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(expires_at - now).count();
        Reply r{redisCommand(ctx_, "SET %b %b PX %lld",
                             key.data(),   static_cast<std::size_t>(key.size()),
                             value.data(), static_cast<std::size_t>(value.size()),
                             static_cast<long long>(ms))};
        (void)r;
    }

    void set_ttl(std::string key, std::string value, Cache::clock::duration ttl) override {
        set_at(std::move(key), std::move(value), Cache::clock::now() + ttl);
    }

    mirobody::optional<std::string> get(const std::string& key) override {
        Reply r{redisCommand(ctx_, "GET %b",
                             key.data(), static_cast<std::size_t>(key.size()))};
        if (!r.r || r.r->type != REDIS_REPLY_STRING) return mirobody::nullopt;
        return std::string(r.r->str, static_cast<std::size_t>(r.r->len));
    }

    mirobody::optional<std::int64_t> incr(const std::string& key) override {
        Reply r{redisCommand(ctx_, "INCR %b",
                             key.data(), static_cast<std::size_t>(key.size()))};
        if (!r.r || r.r->type != REDIS_REPLY_INTEGER) return mirobody::nullopt;
        return static_cast<std::int64_t>(r.r->integer);
    }

    mirobody::optional<std::int64_t> decr(const std::string& key) override {
        Reply r{redisCommand(ctx_, "DECR %b",
                             key.data(), static_cast<std::size_t>(key.size()))};
        if (!r.r || r.r->type != REDIS_REPLY_INTEGER) return mirobody::nullopt;
        return static_cast<std::int64_t>(r.r->integer);
    }

    std::size_t rpush(const std::string& key, const std::vector<std::string>& values) override {
        return push("RPUSH", key, values);
    }

    std::size_t lpush(const std::string& key, const std::vector<std::string>& values) override {
        return push("LPUSH", key, values);
    }

    mirobody::optional<std::string> lpop(const std::string& key) override {
        Reply r{redisCommand(ctx_, "LPOP %b",
                             key.data(), static_cast<std::size_t>(key.size()))};
        if (!r.r || r.r->type != REDIS_REPLY_STRING) return mirobody::nullopt;
        return std::string(r.r->str, static_cast<std::size_t>(r.r->len));
    }

    mirobody::optional<std::string> rpop(const std::string& key) override {
        Reply r{redisCommand(ctx_, "RPOP %b",
                             key.data(), static_cast<std::size_t>(key.size()))};
        if (!r.r || r.r->type != REDIS_REPLY_STRING) return mirobody::nullopt;
        return std::string(r.r->str, static_cast<std::size_t>(r.r->len));
    }

    std::vector<std::string> lrange(const std::string& key,
                                    std::int64_t start, std::int64_t stop) override {
        Reply r{redisCommand(ctx_, "LRANGE %b %lld %lld",
                             key.data(), static_cast<std::size_t>(key.size()),
                             static_cast<long long>(start),
                             static_cast<long long>(stop))};
        std::vector<std::string> out;
        if (!r.r || r.r->type != REDIS_REPLY_ARRAY) return out;
        out.reserve(r.r->elements);
        for (std::size_t i = 0; i < r.r->elements; ++i) {
            const redisReply* e = r.r->element[i];
            if (e && e->type == REDIS_REPLY_STRING) {
                out.push_back(std::string(e->str, static_cast<std::size_t>(e->len)));
            }
        }
        return out;
    }

    void ltrim(const std::string& key, std::int64_t start, std::int64_t stop) override {
        Reply r{redisCommand(ctx_, "LTRIM %b %lld %lld",
                             key.data(), static_cast<std::size_t>(key.size()),
                             static_cast<long long>(start),
                             static_cast<long long>(stop))};
        (void)r;
    }

    mirobody::optional<std::string> set_join(const std::string& dest, const std::string& list,
                                             const std::string& prefix, const std::string& sep,
                                             const std::string& suffix, Cache::clock::duration ttl) override {
        // Read-render-write as one atomic server-side step. EVAL (not
        // EVALSHA): the call is rare enough that resending the script
        // beats managing the script cache across reconnects.
        static const char kScript[] =
            "local rows = redis.call('LRANGE', KEYS[2], 0, -1)\n"
            "if #rows == 0 then return false end\n"
            "local out = ARGV[1] .. table.concat(rows, ARGV[2]) .. ARGV[3]\n"
            "redis.call('SET', KEYS[1], out, 'PX', ARGV[4])\n"
            "return out\n";
        const long long ms = std::max<long long>(
            1, std::chrono::duration_cast<std::chrono::milliseconds>(ttl).count());
        const std::string ms_s = std::to_string(ms);

        const char* argv[9];
        std::size_t  lens[9];
        argv[0] = "EVAL";         lens[0] = 4;
        argv[1] = kScript;        lens[1] = sizeof(kScript) - 1;
        argv[2] = "2";            lens[2] = 1;
        argv[3] = dest.data();    lens[3] = dest.size();
        argv[4] = list.data();    lens[4] = list.size();
        argv[5] = prefix.data();  lens[5] = prefix.size();
        argv[6] = sep.data();     lens[6] = sep.size();
        argv[7] = suffix.data();  lens[7] = suffix.size();
        argv[8] = ms_s.data();    lens[8] = ms_s.size();
        Reply r{redisCommandArgv(ctx_, 9, argv, lens)};
        if (!r.r || r.r->type != REDIS_REPLY_STRING) return mirobody::nullopt;
        return std::string(r.r->str, static_cast<std::size_t>(r.r->len));
    }

    bool exists(const std::string& key) override {
        Reply r{redisCommand(ctx_, "EXISTS %b",
                             key.data(), static_cast<std::size_t>(key.size()))};
        if (!r.r || r.r->type != REDIS_REPLY_INTEGER) return false;
        return r.r->integer > 0;
    }

    bool del(const std::string& key) override {
        Reply r{redisCommand(ctx_, "DEL %b",
                             key.data(), static_cast<std::size_t>(key.size()))};
        if (!r.r || r.r->type != REDIS_REPLY_INTEGER) return false;
        return r.r->integer > 0;
    }

    bool expire_at(const std::string& key, Cache::clock::time_point expires_at) override {
        // Mirror set_at's mapping of the time-point domain: max() means
        // "no TTL" (PERSIST) and a deadline at or before now collapses to
        // DEL rather than a PEXPIRE the server would round to 0.
        if (expires_at == Cache::clock::time_point::max()) {
            Reply r{redisCommand(ctx_, "PERSIST %b",
                                 key.data(), static_cast<std::size_t>(key.size()))};
            return r.r && r.r->type == REDIS_REPLY_INTEGER && r.r->integer > 0;
        }
        const auto now = Cache::clock::now();
        if (expires_at <= now) return del(key);
        const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(expires_at - now).count();
        Reply r{redisCommand(ctx_, "PEXPIRE %b %lld",
                             key.data(), static_cast<std::size_t>(key.size()),
                             static_cast<long long>(ms))};
        return r.r && r.r->type == REDIS_REPLY_INTEGER && r.r->integer > 0;
    }

    mirobody::optional<Cache::clock::time_point> expiretime(const std::string& key) override {
        Reply r{redisCommand(ctx_, "PTTL %b",
                             key.data(), static_cast<std::size_t>(key.size()))};
        if (!r.r || r.r->type != REDIS_REPLY_INTEGER) return mirobody::nullopt;
        const long long ms = r.r->integer;
        if (ms == -2) return mirobody::nullopt;                 // key absent
        if (ms == -1) return Cache::clock::time_point::max();   // key present, no TTL
        return Cache::clock::now() + std::chrono::milliseconds(ms);
    }

    std::size_t prune() override {
        // Redis evicts expired keys internally; no public command to force it.
        return 0;
    }

    void flushdb() override {
        Reply r{redisCommand(ctx_, "FLUSHDB")};
        (void)r;
    }

    std::size_t dbsize() override {
        Reply r{redisCommand(ctx_, "DBSIZE")};
        if (!r.r || r.r->type != REDIS_REPLY_INTEGER) return 0;
        return static_cast<std::size_t>(r.r->integer);
    }

private:
    // RPUSH / LPUSH differ only in the command word. Variable argument
    // count: assemble argv for redisCommandArgv (length-prefixed, so the
    // values are binary-safe like %b).
    std::size_t push(const char* cmd, const std::string& key,
                     const std::vector<std::string>& values) {
        if (values.empty()) return 0;
        std::vector<const char*> argv;
        std::vector<std::size_t> lens;
        argv.reserve(values.size() + 2);
        lens.reserve(values.size() + 2);
        argv.push_back(cmd);         lens.push_back(std::strlen(cmd));
        argv.push_back(key.data());  lens.push_back(key.size());
        for (std::size_t i = 0; i < values.size(); ++i) {
            argv.push_back(values[i].data());
            lens.push_back(values[i].size());
        }
        Reply r{redisCommandArgv(ctx_, static_cast<int>(argv.size()),
                                 argv.data(), lens.data())};
        if (!r.r || r.r->type != REDIS_REPLY_INTEGER) return 0;
        return static_cast<std::size_t>(r.r->integer);
    }

#if defined(MIROBODY_HAS_HIREDIS_SSL)
    // Build an OpenSSL-backed TLS context from the RedisConfig. Throws
    // std::runtime_error if the context can't be created. Caller owns the
    // result; free_ssl_context() releases it.
    static redisSSLContext* make_ssl_context(const RedisConfig& cfg) {
        // OpenSSL needs a one-time library init before the first TLS use.
        static std::once_flag once;
        std::call_once(once, []() { redisInitOpenSSL(); });

        // Map redis-py-style ssl_cert_reqs onto hiredis verify modes. An unset
        // value defaults to full verification (secure by default).
        int verify_mode;
        if (cfg.ssl_cert_reqs == "none") {
            verify_mode = REDIS_SSL_VERIFY_NONE;
        } else if (cfg.ssl_cert_reqs == "optional") {
            verify_mode = REDIS_SSL_VERIFY_PEER;
        } else {  // "required" or unset
            verify_mode = REDIS_SSL_VERIFY_PEER | REDIS_SSL_VERIFY_FAIL_IF_NO_PEER_CERT;
        }

        // server_name drives both SNI and certificate hostname checking in
        // hiredis: set it to the host only when hostname verification is wanted,
        // otherwise leave it null so the handshake doesn't enforce a CN/SAN
        // match. CA trust falls back to OpenSSL's default verify paths (the
        // system ca-certificates bundle), since no explicit CA/client cert is
        // exposed in the config surface.
        redisSSLOptions opts{};
        opts.cacert_filename      = nullptr;
        opts.capath               = nullptr;
        opts.cert_filename        = nullptr;
        opts.private_key_filename = nullptr;
        opts.server_name          = cfg.ssl_check_hostname ? cfg.host.c_str() : nullptr;
        opts.verify_mode          = verify_mode;

        redisSSLContextError err = REDIS_SSL_CTX_NONE;
        redisSSLContext* ssl = redisCreateSSLContextWithOptions(&opts, &err);
        if (!ssl) {
            throw std::runtime_error(
                std::string{"cache::Cache: TLS context: "} + redisSSLContextGetError(err));
        }
        return ssl;
    }

    void free_ssl_context() {
        if (ssl_ctx_) {
            redisFreeSSLContext(ssl_ctx_);
            ssl_ctx_ = nullptr;
        }
    }

    redisSSLContext* ssl_ctx_ = nullptr;
#else
    void free_ssl_context() {}
#endif

    redisContext* ctx_ = nullptr;
};

}

//------------------------------------------------------------------------------
// Cache front
//------------------------------------------------------------------------------

Cache MemoryKvConfig::open() const {
    return Cache{};
}

//------------------------------------------------------------------------------

Cache RedisConfig::open() const {
    return Cache{*this};
}

//------------------------------------------------------------------------------

Cache::Cache() : impl_(new MemoryImpl()) {}

//------------------------------------------------------------------------------

Cache::Cache(const RedisConfig& cfg) : impl_(new RedisImpl(cfg)) {}

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

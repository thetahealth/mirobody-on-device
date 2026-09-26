#include "cache/cache.hpp"
#include "cache/memory_kv.hpp"

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <cstdint>
#include <limits>
#include <stdexcept>
#include <string>
#include <thread>

using mirobody::cache::Cache;
using mirobody::cache::MemoryKv;
using mirobody::cache::MemoryKvConfig;

//------------------------------------------------------------------------------

TEST_CASE("MemoryKv set/get round trip", "[cache]") {
    MemoryKv kv;
    kv.set("foo", "bar", MemoryKv::clock::time_point::max());
    auto v = kv.get("foo");
    REQUIRE(v.has_value());
    REQUIRE(*v == "bar");
    REQUIRE_FALSE(kv.get("missing").has_value());
}

//------------------------------------------------------------------------------

TEST_CASE("MemoryKv past-expiration entries are treated as absent", "[cache]") {
    MemoryKv kv;
    auto past = MemoryKv::clock::now() - std::chrono::milliseconds(1);
    kv.set("foo", "bar", past);
    REQUIRE_FALSE(kv.get("foo").has_value());
    REQUIRE_FALSE(kv.exists("foo"));
}

//------------------------------------------------------------------------------

TEST_CASE("MemoryKv TTL via duration overload", "[cache]") {
    MemoryKv kv;
    kv.set("foo", "bar", std::chrono::milliseconds(50));
    REQUIRE(kv.exists("foo"));
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    REQUIRE_FALSE(kv.exists("foo"));
}

//------------------------------------------------------------------------------

TEST_CASE("MemoryKv del removes the key and reports prior presence", "[cache]") {
    MemoryKv kv;
    kv.set("foo", "bar", MemoryKv::clock::time_point::max());
    REQUIRE(kv.del("foo"));
    REQUIRE_FALSE(kv.del("foo"));   // already gone
    REQUIRE_FALSE(kv.exists("foo"));
}

//------------------------------------------------------------------------------

TEST_CASE("MemoryKv expiretime reflects the stored expiration", "[cache]") {
    MemoryKv kv;
    kv.set("persistent", "1", MemoryKv::clock::time_point::max());
    auto t1 = kv.expiretime("persistent");
    REQUIRE(t1.has_value());
    REQUIRE(*t1 == MemoryKv::clock::time_point::max());

    const auto deadline = MemoryKv::clock::now() + std::chrono::seconds(30);
    kv.set("ttl_key", "2", deadline);
    REQUIRE(kv.expiretime("ttl_key").value() == deadline);

    REQUIRE_FALSE(kv.expiretime("missing").has_value());
}

//------------------------------------------------------------------------------

TEST_CASE("MemoryKv incr / decr match Redis semantics", "[cache]") {
    MemoryKv kv;
    // Missing key is created at 0 first.
    REQUIRE(kv.incr("counter").value() == 1);
    REQUIRE(kv.incr("counter").value() == 2);
    REQUIRE(kv.decr("counter").value() == 1);

    // Non-integer value is rejected.
    kv.set("text", "hello", MemoryKv::clock::time_point::max());
    REQUIRE_FALSE(kv.incr("text").has_value());

    // Overflow is rejected (no wrap-around).
    kv.set("big",
           std::to_string(std::numeric_limits<std::int64_t>::max()),
           MemoryKv::clock::time_point::max());
    REQUIRE_FALSE(kv.incr("big").has_value());

    kv.set("small",
           std::to_string(std::numeric_limits<std::int64_t>::min()),
           MemoryKv::clock::time_point::max());
    REQUIRE_FALSE(kv.decr("small").has_value());
}

//------------------------------------------------------------------------------

TEST_CASE("MemoryKv incr preserves an existing TTL", "[cache]") {
    MemoryKv kv;
    const auto deadline = MemoryKv::clock::now() + std::chrono::seconds(30);
    kv.set("c", "1", deadline);
    REQUIRE(kv.incr("c").value() == 2);
    REQUIRE(kv.expiretime("c").value() == deadline);
}

//------------------------------------------------------------------------------

TEST_CASE("MemoryKv rpush / lrange / ltrim follow Redis list semantics", "[cache]") {
    MemoryKv kv;
    REQUIRE(kv.lrange("missing", 0, -1).empty());
    REQUIRE(kv.rpush("l", {"a", "b"}) == 2);
    REQUIRE(kv.rpush("l", {"c"}) == 3);

    REQUIRE(kv.lrange("l", 0, -1) == std::vector<std::string>{"a", "b", "c"});
    REQUIRE(kv.lrange("l", -2, -1) == std::vector<std::string>{"b", "c"});
    REQUIRE(kv.lrange("l", 1, 1) == std::vector<std::string>{"b"});
    REQUIRE(kv.lrange("l", 1, 100) == std::vector<std::string>{"b", "c"});   // stop clamped
    REQUIRE(kv.lrange("l", 5, 9).empty());                                   // past the end
    REQUIRE(kv.lrange("l", 2, 1).empty());                                   // inverted

    kv.ltrim("l", -2, -1);   // keep the newest two, as a size cap would
    REQUIRE(kv.lrange("l", 0, -1) == std::vector<std::string>{"b", "c"});

    kv.ltrim("l", 1, 0);     // empty range removes the key, as in Redis
    REQUIRE_FALSE(kv.exists("l"));
}

//------------------------------------------------------------------------------

TEST_CASE("MemoryKv lpush / lpop / rpop follow Redis list semantics", "[cache]") {
    MemoryKv kv;
    REQUIRE_FALSE(kv.lpop("missing").has_value());
    REQUIRE_FALSE(kv.rpop("missing").has_value());

    // LPUSH prepends one value at a time: the last argument ends up first.
    REQUIRE(kv.lpush("l", {"a", "b"}) == 2);
    REQUIRE(kv.lrange("l", 0, -1) == std::vector<std::string>{"b", "a"});
    REQUIRE(kv.lpush("l", {"c"}) == 3);
    REQUIRE(kv.lrange("l", 0, -1) == std::vector<std::string>{"c", "b", "a"});

    REQUIRE(kv.lpop("l").value() == "c");
    REQUIRE(kv.rpop("l").value() == "a");
    REQUIRE(kv.lrange("l", 0, -1) == std::vector<std::string>{"b"});

    // Popping the last element removes the key, as in Redis.
    REQUIRE(kv.rpop("l").value() == "b");
    REQUIRE_FALSE(kv.exists("l"));

    // A queue shape: rpush producers, lpop consumer (FIFO).
    kv.rpush("q", {"1", "2", "3"});
    REQUIRE(kv.lpop("q").value() == "1");
    REQUIRE(kv.lpop("q").value() == "2");
    REQUIRE(kv.lpop("q").value() == "3");
    REQUIRE_FALSE(kv.lpop("q").has_value());
}

//------------------------------------------------------------------------------

TEST_CASE("MemoryKv list / string type mismatches mirror WRONGTYPE", "[cache]") {
    MemoryKv kv;
    kv.set("s", "x", MemoryKv::clock::time_point::max());
    REQUIRE(kv.rpush("s", {"a"}) == 0);
    REQUIRE(kv.lpush("s", {"a"}) == 0);
    REQUIRE_FALSE(kv.lpop("s").has_value());
    REQUIRE_FALSE(kv.rpop("s").has_value());
    REQUIRE(kv.lrange("s", 0, -1).empty());
    kv.ltrim("s", 0, 0);                       // no-op, not an erase
    REQUIRE(kv.get("s").value() == "x");

    REQUIRE(kv.rpush("l", {"a"}) == 1);
    REQUIRE_FALSE(kv.get("l").has_value());
    REQUIRE_FALSE(kv.incr("l").has_value());
    REQUIRE(kv.exists("l"));                   // type-agnostic ops still work
    REQUIRE(kv.del("l"));

    // SET replaces a list wholesale, as in Redis.
    kv.rpush("r", {"a"});
    kv.set("r", "v", MemoryKv::clock::time_point::max());
    REQUIRE(kv.get("r").value() == "v");
    REQUIRE(kv.lrange("r", 0, -1).empty());
}

//------------------------------------------------------------------------------

TEST_CASE("MemoryKv set_join renders a list into a string key", "[cache]") {
    MemoryKv kv;
    kv.rpush("l", {"a", "b", "c"});
    auto out = kv.set_join("j", "l", "[", ",", "]", std::chrono::seconds(60));
    REQUIRE(out.value() == "[a,b,c]");
    REQUIRE(kv.get("j").value() == "[a,b,c]");
    REQUIRE(kv.expiretime("j").has_value());   // TTL applied

    // Absent / empty / string-typed sources render nothing; dest untouched.
    REQUIRE_FALSE(kv.set_join("j2", "missing", "[", ",", "]", std::chrono::seconds(60)).has_value());
    REQUIRE_FALSE(kv.exists("j2"));
    kv.set("s", "x", MemoryKv::clock::time_point::max());
    REQUIRE_FALSE(kv.set_join("j2", "s", "[", ",", "]", std::chrono::seconds(60)).has_value());
    REQUIRE_FALSE(kv.exists("j2"));

    // A single row gets no separator, just the affixes.
    kv.rpush("one", {"x"});
    REQUIRE(kv.set_join("j3", "one", "<", "|", ">", std::chrono::seconds(60)).value() == "<x>");
}

//------------------------------------------------------------------------------

TEST_CASE("MemoryKv expire re-anchors an existing key's TTL", "[cache]") {
    MemoryKv kv;
    REQUIRE_FALSE(kv.expire("missing", std::chrono::seconds(30)));

    const auto deadline = MemoryKv::clock::now() + std::chrono::seconds(30);
    kv.set("k", "v", MemoryKv::clock::time_point::max());
    REQUIRE(kv.expire("k", deadline));
    REQUIRE(kv.expiretime("k").value() == deadline);

    // Clearing a TTL reports true only when there was one (PERSIST).
    REQUIRE(kv.expire("k", MemoryKv::clock::time_point::max()));
    REQUIRE_FALSE(kv.expire("k", MemoryKv::clock::time_point::max()));

    // Lists carry TTLs the same way; an rpush-created list has none.
    kv.rpush("l", {"a"});
    REQUIRE(kv.expiretime("l").value() == MemoryKv::clock::time_point::max());
    REQUIRE(kv.expire("l", deadline));
    REQUIRE(kv.expiretime("l").value() == deadline);

    // A past deadline expires the key immediately.
    REQUIRE(kv.expire("l", MemoryKv::clock::now() - std::chrono::milliseconds(1)));
    REQUIRE_FALSE(kv.exists("l"));
}

//------------------------------------------------------------------------------

TEST_CASE("MemoryKv prune removes expired entries", "[cache]") {
    MemoryKv kv;
    kv.set("expired", "1", MemoryKv::clock::now() - std::chrono::milliseconds(1));
    kv.set("kept",    "2", MemoryKv::clock::time_point::max());
    REQUIRE(kv.dbsize() == 2);     // not yet pruned
    REQUIRE(kv.prune() == 1);
    REQUIRE(kv.dbsize() == 1);
    REQUIRE(kv.get("kept").has_value());
}

//------------------------------------------------------------------------------

TEST_CASE("MemoryKv flushdb empties the store", "[cache]") {
    MemoryKv kv;
    kv.set("a", "1", MemoryKv::clock::time_point::max());
    kv.set("b", "2", MemoryKv::clock::time_point::max());
    REQUIRE_FALSE(kv.empty());
    kv.flushdb();
    REQUIRE(kv.empty());
    REQUIRE(kv.dbsize() == 0);
}

//------------------------------------------------------------------------------

TEST_CASE("MemoryKv::open returns the same singleton on every call", "[cache]") {
    auto& a = MemoryKv::open();
    auto& b = MemoryKv::open();
    REQUIRE(&a == &b);
}

//------------------------------------------------------------------------------

TEST_CASE("MemoryKv background sweeper evicts without manual prune", "[cache]") {
    MemoryKv kv;
    kv.set("a", "1", std::chrono::milliseconds(50));
    REQUIRE(kv.dbsize() == 1);
    // Sweeper runs once per second. Wait slightly past that so it has at
    // least one full pass to fire.
    std::this_thread::sleep_for(std::chrono::milliseconds(1500));
    REQUIRE(kv.dbsize() == 0);
}

//------------------------------------------------------------------------------

TEST_CASE("Cache default-constructed proxies a MemoryKv backend", "[cache]") {
    Cache c;
    c.set("k", "v", Cache::clock::time_point::max());
    REQUIRE(c.exists("k"));
    REQUIRE(c.get("k").value() == "v");
    REQUIRE(c.incr("n").value() == 1);
    REQUIRE(c.dbsize() == 2);
    REQUIRE(c.del("k"));
    c.flushdb();
    REQUIRE(c.empty());
}

//------------------------------------------------------------------------------

TEST_CASE("Cache proxies the list operations", "[cache]") {
    Cache c;
    REQUIRE(c.rpush("l", "a") == 1);
    REQUIRE(c.rpush("l", std::vector<std::string>{"b", "c"}) == 3);
    c.ltrim("l", -2, -1);
    REQUIRE(c.lrange("l", 0, -1) == std::vector<std::string>{"b", "c"});
    REQUIRE(c.lpush("l", "a") == 3);
    REQUIRE(c.lpush("l", std::vector<std::string>{"z", "y"}) == 5);
    REQUIRE(c.lpop("l").value() == "y");
    REQUIRE(c.rpop("l").value() == "c");
    REQUIRE(c.lrange("l", 0, -1) == std::vector<std::string>{"z", "a", "b"});
    REQUIRE(c.expire("l", std::chrono::seconds(60)));
    REQUIRE(c.expiretime("l").has_value());
    REQUIRE(c.set_join("j", "l", "[", ",", "]", std::chrono::seconds(60)).value() == "[z,a,b]");
    REQUIRE(c.get("j").value() == "[z,a,b]");
}

//------------------------------------------------------------------------------

TEST_CASE("MemoryKvConfig::open builds a working Cache", "[cache]") {
    auto c = MemoryKvConfig{}.open();
    c.set("foo", "bar", std::chrono::seconds(60));
    REQUIRE(c.get("foo").value() == "bar");
}


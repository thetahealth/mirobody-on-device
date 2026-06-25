# Cache

`mirobody::cache::Cache` is a Redis-flavored key/value front (`set` / `get` /
`incr` / `decr` / `exists` / `del` / `expiretime` / `prune` / `flushdb` /
`dbsize`) that wraps either an in-process `MemoryKv` or a hiredis-backed
Redis connection behind one type. Pick a backend through the matching
config — both share the same `Cache open() const` shape, so call sites
read the same regardless of backend:

```cpp
auto cfg = mirobody::load_config();
auto mem = cfg.memory_kv.open();   // in-process; one sweeper thread per instance
auto rdb = cfg.redis.open();       // hiredis-backed
```

Expirations are anchored to `std::chrono::steady_clock`, so wall-clock
adjustments cannot resurrect or prematurely evict entries. The in-process
backend evicts lazily on lookup and proactively via a 1-second background
sweeper per `MemoryKv` instance; the Redis backend translates TTLs to
`PX` and lets the server handle eviction. `cache::MemoryKv` stays public
for callers that prefer direct access, with a process-wide singleton at
`MemoryKv::open()`.

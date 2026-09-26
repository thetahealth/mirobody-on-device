# Cache

`mirobody::cache::Cache` is a Redis-flavored key/value front (`set` / `get` /
`incr` / `decr` / `exists` / `del` / `expiretime` / `prune` / `flushdb` /
`dbsize`) over an in-process `MemoryKv`. The method names follow Redis because the
server-side cache in the [main mirobody repo](https://github.com/thetahealth/mirobody)
is Redis, which keeps call sites readable to anyone who knows either; the Redis
backend itself left with the server (it is preserved at the `v2-full-2026-08` tag).

```cpp
auto cfg = mirobody::load_config();
auto mem = cfg.memory_kv.open();   // in-process; one sweeper thread per instance
```

Expirations are anchored to `std::chrono::steady_clock`, so wall-clock
adjustments cannot resurrect or prematurely evict entries. The store evicts
lazily on lookup and proactively via a 1-second background sweeper per
`MemoryKv` instance. `cache::MemoryKv` stays public for callers that prefer
direct access, with a process-wide singleton at `MemoryKv::open()`.

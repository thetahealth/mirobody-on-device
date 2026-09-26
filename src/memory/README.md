# Memory

`mirobody::memory::Memory` is the long-term memory interface: it stores durable
facts about a user and recalls the most relevant ones for a turn. It is the
mirobody analog of [EverOS](https://github.com/EverMind-AI/EverOS)'s memory
subsystem, pared down to the slice that matters here — per-user long-term
recall, surfaced to agents as the `remember` / `recall_memory` MCP tools
([res/mcp_tools/](../../res/mcp_tools/)).

```cpp
auto cfg    = mirobody::load_config();
auto db     = cfg.sqlite.open();
auto memory = mirobody::memory::make_memory(cfg, db);   // backend per MEMORY_PROVIDER

std::string err;
memory->remember({/*user_id*/ 42, "prefers metric units", "preference", ""}, &err);
auto hits = memory->recall(42, "what units does the user like?", 5, &err);
// hits[0].text == "prefers metric units", hits[0].score is the cosine match
```

The interface is three methods — `remember`, `recall`, `forget` — all taking the
caller's `user_id`, so one store serves every user. Failures are reported by
return value + `*err` rather than thrown, so a tool handler surfaces a clean
message. `make_memory()` returns null when memory is disabled; the tools treat null as "memory unavailable".

## Backends

| `MEMORY_PROVIDER`   | Backend       | Notes |
|---------------------|---------------|-------|
| `local` *(default)* | `LocalMemory` | Facts + 1024-dim embeddings in the SQLite file; recall ranks the caller's own rows by in-process cosine. No extra services. |
| `none`              | *(disabled)*  | The tools report memory is unavailable. |

The hosted memory services (EverOS, Mem0, Zep) send the facts to a third party,
which the phone's record must not do by default, so their adapters were removed;
they are preserved at the `v2-full-2026-08` tag.

### LocalMemory

The built-in, no-extra-services backend. Each memory is one row in the
`memories` table (see [res/sql/](../../res/sql/)), with the embedding stored
verbatim as a little-endian float32 BLOB next to the text. `recall` loads **only
the caller's active rows** (a `WHERE user_id = ? AND deleted_at IS NULL` filter —
`forget` is a soft delete that stamps `deleted_at`), de-serializes each vector,
and ranks by cosine similarity in process — so the working set is small (a few MB
for thousands of memories) and no vector index is needed.

The embedder is injected (`EmbedFn`) rather than called directly, so the cosine
ranking is unit-tested with a deterministic stub instead of the network
([tests/memory/memory_test.cpp](../../tests/memory/memory_test.cpp));
`make_local_memory()` binds the real, config-driven `embedding::text_embedding_one`.

> **Scaling.** Per-user in-process cosine is the simple-but-effective default. A
> `pgvector` (`vector(1024)` + ANN index) or `sqlite-vec` backend that pushes the
> search into SQL slots in behind this same interface once one user accumulates
> tens of thousands of memories or you need cross-user search.

## Configuration

| Key | Meaning |
|-----|---------|
| `MEMORY_PROVIDER`  | `local` (default) / `none`. |
| `MEMORY_TOP_K`     | Default recall count when the caller omits one (default 5). |

## Wiring

The server constructs one `Memory` at startup and shares the pointer with the
chat agent's tool executor and the MCP endpoint — both reach it through
`mcp::ToolContext` (alongside `cache` / `storage` / `db`). The plumbing mirrors
how `storage` is threaded: `AgentRequest` → `llm::UserContext` → `ToolContext`
on the agent path, and `McpService` builds the `ToolContext` directly on the
`/mcp` path.

## Adding a backend

1. Add `make_<name>_memory(const Config&)` (and a file-local `Memory` subclass)
   in `src/memory/<name>_memory.{hpp,cpp}`, modeled on `local_memory.cpp`.
2. Dispatch it from `make_memory()` in `memory.cpp` on a new `MEMORY_PROVIDER`
   value.
3. Register the `.cpp` in the root `CMakeLists.txt` (`MIROBODY_CORE_SOURCES`).

No change to the tool layer or the `Memory` interface is needed.

## Not yet built

Automatic LLM-driven fact extraction from chat turns (today `LocalMemory`
captures only via the explicit `remember` tool), agent cases/skills, and multimodal ingestion — the parts of full
EverOS parity beyond per-user long-term recall.

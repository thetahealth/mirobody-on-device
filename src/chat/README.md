# `src/chat` — the chat service

The chat service is organized into four tiers. Each tier depends only on the
tiers below it, so transport concerns never leak into the domain and the domain
never knows about HTTP/WebSocket framing.

```
Tier 1  Interface   service.* transport/* dispatcher.* params.* event/* packet.*
Tier 2  Chat        chat.*                              response / live / persist
Tier 3  Agent       agent.*                             pluggable agents (res/agents/*)
Tier 4  MCP tools   src/mcp/*                           pluggable tools (res/mcp_tools/*)
```

## Tier 1 — interface (transport)

The transport front end. A **transport** does only the two boundary translations
for one wire protocol — `bytes → Packet` on the way in, `llm::Event → bytes` on
the way out — and nothing else. It does *not* run the turn: it hands the `Packet`
plus a `Responder` (its output channel) to the `Dispatcher`, which drives the
domain. So a transport knows only its wire, `Packet`, and `Event`; adding one
(e.g. MQTT) is additive and touches nothing else.

```
bytes ──transport.parse──▶ Packet ──Dispatcher──▶ Event stream ──Responder──▶ bytes
       (transport)                   (→ Chat)                     (transport)
```

- **`transport/transport.hpp`** — the abstract `Transport` interface. Fixes only
  the lifecycle (`name()`, `start()`); transports share nothing at the connection
  level (HTTP/WS use the `Router`, MQTT talks to a broker).
- **`transport/responder.hpp`** — the abstract `Responder`: a request's outbound
  channel. `send(Event)` serializes+writes one event (false ⇒ client gone);
  `finish()` emits the terminal `{"type":"end"}`. One impl per transport.
- **`transport/sse.{hpp,cpp}`** — `SseTransport` + `SseResponder`. `POST /api/chat`
  → a `kOpChat` `Packet`, attaching any uploaded file bytes as Packet attachments
  (the dispatcher stores them, not the transport) → **Server-Sent Events**.
  An upload's size is bounded before it ever reaches here: the router caps the
  whole request body at `HTTP_MAX_BODY_BYTES` (32 MiB by default) and answers
  `413`, since the body is buffered whole and each attachment is then copied
  down this chain — see [src/config/README.md](../config/README.md#request-body-limit).
- **`transport/ws.{hpp,cpp}`** — `WsTransport` + `WsResponder`. `GET /api/chat` →
  **WebSocket** (JWT-guarded handshake); each frame becomes a `kOpLive` `Packet`,
  run on a worker thread. Owns the per-connection `LiveSession` state.
- **`transport/mqtt.{hpp,cpp}`** — `MqttTransport`. Placeholder; `start()` is inert
  until a broker client is built. Documents the integration points.
- **`dispatcher.{hpp,cpp}`** — `Dispatcher`. The seam between transports and
  `Chat`. First applies the **per-user rate limit** (`CHAT_RATE_MAX` /
  `CHAT_RATE_WINDOW_SEC`, via the cache) and rejects an over-limit caller before
  any work — covering both the SSE and WebSocket paths since both arrive here;
  `0` disables it (the shipped `config.example.yml` caps it at 5 turns / 60s).
  Then runs an **upload preprocess** (`store_attachments`): offloads each
  of the Packet's binary attachments to object storage and records a reference in
  `params.files`, so bytes never enter the JSON — emitting a `UploadEvent`
  per file (and a `TranscriptEvent` begin/done pair around each text extraction)
  so the client sees progress. Then switches on `Packet::code()`
  (`kOpChat` / `kOpLive`), parses the typed params struct, calls the matching
  `Chat` method, and streams the events into the `Responder`. Owns the
  `storage::Storage` handle. Transport-neutral and reentrant.
- **`params.{hpp,cpp}`** — per-command typed parameters: `ChatParams` / `LiveParams`,
  each with a `parse(pkt)` that turns the generic `Packet` into a validated,
  named-field request (`AgentRequest` / `LiveRequest`). Adding a command = a struct
  here + a case in the dispatcher; the wire boundary stays generic.
- **`service.{hpp,cpp}`** — `ChatService`, the **composition root**. Constructs the
  `Chat` and `Dispatcher`, owns one transport per enabled protocol and starts
  them, and registers the non-streaming REST routes itself: `GET|POST /api/providers`,
  `GET|POST /api/history`, `POST /api/history/delete`, `GET|POST /api/files`.
  `/api/history` returns `summaries[]`, each entry carrying `session_id`, `timestamp`,
  `summary`, `query_user_id`, `owned`, and `message_count`; an owned entry adds
  `shared_with_count`, a shared-to-me one adds `shared_by`. **`message_count` is two
  subqueries, not one**: a conversation's opening question has `conversation_id NULL`
  (its own id *is* the conversation id — see
  [res/sql/pg/1_chat.sql](../../res/sql/pg/1_chat.sql)), so counting only the
  `conversation_id` matches is short by exactly one on every thread. They are written
  as two index-friendly probes rather than the `(id=? OR conversation_id=?)` the
  detail query uses, because this one runs per row of the page.
  `/api/files` lists the caller's uploads from the `files` table (the queryable
  index; see [res/sql/pg/1_chat.sql](../../res/sql/pg/1_chat.sql) and
  `file::db_list_files`): `?page` / `?size` (default 0 / 20, max 100),
  `?sort=time|name` (default time), `?order=asc|desc` (default desc), returning
  `total` / `page` / `size`. Each entry carries `filename`, `mime_type`,
  `file_key`, `summary` (empty until post-upload processing fills it), and
  browser-fetchable signed URLs — `url` for the raw bytes and `text_url` for the
  extracted text when one exists (minted by `Storage::signed_read_url`, only for
  the page's entries, so a plain `<img src>` / download link works with no auth
  header; the list call itself is JWT-guarded like the others). The upload path
  (`chat::Dispatcher`) inserts each row, then fills `text_key` once extraction
  finishes; the object-storage `.meta` sidecars remain the durable truth.
- **`packet.{hpp,cpp}`** — `Packet`, the `{code, params}` request envelope a
  transport produces and the dispatcher consumes; one shape across transports. Also
  carries optional binary `Attachment`s (raw uploaded bytes, kept out of the JSON
  params) that the dispatcher's preprocess offloads to object storage.
- **`event/event.{hpp,cpp}`** — the abstract `chat::Event` and its concrete,
  self-serializing subclasses (`ReplyEvent`, `ThinkingEvent`, `ToolEvent`,
  `ChartEvent`, `CostEvent`, `ErrorEvent`, `UploadEvent`, `TranscriptEvent`,
  `EndEvent`) — the chat service's *outbound* vocabulary. `event_from_llm()`
  translates the domain's `llm::Event` stream into these at the dispatcher
  boundary; a `Responder` only ever sees `chat::Event`. A few have no `llm`
  counterpart and are emitted directly by the dispatcher: `EndEvent` (from
  `Responder::finish()`) and `UploadEvent` / `TranscriptEvent` (from the
  upload preprocess, one per uploaded file, before the turn streams). Distinct
  from `llm::Event` (the producer type in `src/llm`), which the lower layer owns.
- **`event/filter/filter.{hpp,cpp}`** — `EventFilter` (a stream transform over
  `chat::Event`) + `EventPipeline`. The dispatcher runs the translated events
  through `make_event_pipeline()` before the Responder. **Adding a presentation
  event is chat-tier only:** write an `EventFilter` subclass under
  `event/filter/`, add a line to `make_event_pipeline()`, and (if it emits a new
  shape) an `Event` subclass — no `src/llm` or transport changes.
- **`event/filter/chart.{hpp,cpp}`** — `ChartFilter`: collapses a `render_chart`
  tool call (the `ToolEvent` Title/Arguments/Detail triple) into one `ChartEvent`.
  The worked example of the filter recipe above.

## Tier 2 — `Chat` (domain)

- **`chat.{hpp,cpp}`** — `Chat`. The transport-agnostic core. It deals in
  `llm::Event` (streamed through an `llm::EventHandler`) and plain result structs,
  never in `server::Request`/`Response` or SSE/JSON frames. Owns the turn engine:
  `response()`, `live_response()`, and `persist_history()` (the one history write —
  per turn it inserts the opening question into `messages` and seeds a thin
  `conversations` row keyed on that question's id; the chat schema and its
  per-backend table/key bridge are documented in
  [`../database/README.md`](../database/README.md#chat-history)).
  Borrows `Config` and `Database`.
  The read-only REST operations — provider discovery and history list/delete —
  live on `ChatService` instead (reached only from REST handlers, not the turn
  flow), talking to the agent registry / DB directly.

## Tier 3 — agent framework

- **`agent.{hpp,cpp}`** — the `Agent` contract, the `AgentRegistry`, and the
  per-agent LLM-client store. Concrete agents live in `res/agents/*.cpp` and
  self-register via `MIROBODY_REGISTER_AGENT`.

## Tier 4 — MCP tool framework

Lives in **`src/mcp/`** (`tool.*` = the registry/contract, `service.*` = the
JSON-RPC `/mcp` endpoint). Concrete tools live in `res/mcp_tools/*.cpp`. Agents
invoke these tools; the MCP service also exposes them over JSON-RPC.

### The "currently for" subject and tools

Tools always execute as the **caller** (`UserInfo::user_id`), never as another
user — this keeps writes (`remember`, `summarize_conversation`) and other-user
data (`list_files`, `recall_memory`) scoped to the authenticated caller. The one
exception is **read-only health delegation**: when a turn carries a care-circle
`subject` (the chat composer's "currently for" picker), the dispatcher resolves
+ access-checks it (`resolve_health_subject`, read-only) and threads it as
`UserInfo::subject_user_id` alongside the caller. Only `family_health` acts on
it (defaulting to that member when no `member` arg is given, still re-gated by
`can_read_health`); `whoami` merely flags `acting_for_member`. No tool ever
swaps its identity to the subject.

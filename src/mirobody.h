// Public C API for embedding mirobody into host applications.
//
// Used by the iOS bridge today; the Android JNI bridge will likely migrate
// to delegate here so both platforms share the same surface. Generic C/C++
// hosts that want to bypass the HTTP/WebSocket front door can also link
// against this header directly.

#pragma once

#include <stddef.h>   // size_t

#ifdef __cplusplus
extern "C" {
#endif

// Opaque handle to a running mirobody server.
typedef struct mirobody_server mirobody_server_t;

// Start a server. Returns NULL on failure.
//
// Both arguments are optional — pass NULL or an empty string to fall back to
// the config file and compiled-in defaults:
//   config_path  YAML config file; NULL/empty => the default discovery
//                (MIROBODY_CONFIG env, then ./config.yml, then defaults).
//   data_dir     directory for on-device state (the SQLite database file);
//                NULL/empty => the configured path.
//
// What used to be passed as explicit overrides — the LLM API keys and the
// listen port — now comes from that config, which also reads the matching
// environment variables (OPENAI_API_KEY / GOOGLE_API_KEY / HTTP_PORT). Read the
// bound port back with mirobody_listen_port().
//
// On success, returns an opaque handle that must be released with
// mirobody_stop().
mirobody_server_t* mirobody_start(
    const char* config_path,
    const char* data_dir);

// Stop a running server and release the handle. Safe to call with NULL.
void mirobody_stop(mirobody_server_t* handle);

// Returns non-zero if the server is currently running.
int mirobody_is_running(mirobody_server_t* handle);

// Returns the port the server is bound to, or -1 if handle is NULL.
int mirobody_listen_port(mirobody_server_t* handle);

//------------------------------------------------------------------------------
// Configuration (embedded hosts)
//------------------------------------------------------------------------------
//
// An embedded host (a mobile app linking the library directly) has no config.yml
// and no shell to export variables from; its configuration lives in platform
// stores (secure key store, preferences). These two calls bridge that world onto
// the process-wide config every other entry point already reads.

// Set one configuration value, by the same UPPER_SNAKE name config.yml uses
// (e.g. "ZHIPU_API_KEY", "LOG_LEVEL"). Implemented as a process environment
// write, which the config loader gives precedence over YAML. Returns 0 on
// success. NULL value unsets the variable.
//
// Values already captured by loaded state do NOT retroactively change: call this
// for everything BEFORE the first chat, or follow up with
// mirobody_reload_providers() to rebuild from the current environment.
int mirobody_set_config(const char* key, const char* value);

// Reload the process-wide config from its sources (environment included) and
// rebuild every agent's provider clients from it. Call after mirobody_set_config
// once chat has already run -- e.g. the user pasted a new API key mid-session.
// Returns the number of providers now loaded for the default agent, or -1 on
// failure. Not safe to call concurrently with an in-flight mirobody_chat turn.
int mirobody_reload_providers(void);

//------------------------------------------------------------------------------
// Chat
//------------------------------------------------------------------------------

// Get the providers that have an LLM client loaded — the set a caller can choose
// from for mirobody_chat. Each line is a token you pass straight to
// mirobody_chat: a bare model for the default agent (e.g. "gpt-5-nano"), or an
// "agent/model" pair for any non-default agent (e.g. "Other/gpt-5-nano"). Uses
// the process-wide config (loaded, and the clients built, automatically on first
// use), so it works whether or not a server is running.
//
// (The HTTP /api/providers endpoint exposes the same set but as agent-grouped
// JSON, [{ "agent": ..., "providers": [...] }]; this C ABI returns the flat
// token form instead.)
//
// Returns a NUL-terminated, newline-separated list (one token per line), or an
// empty string when none are configured. The buffer is owned by mirobody and
// stays valid until the next mirobody_get_providers() call on the same thread —
// copy it if you need to keep it. Returns NULL only on error (client init failure).
const char* mirobody_get_providers(void);

// Streaming callback for mirobody_chat, invoked once per event as the turn
// streams on the calling thread.
//
//   event_type  a stable lowercase tag for the chunk: "reply" (answer text),
//               "thinking" (reasoning), "queryTitle" / "queryArguments" /
//               "queryDetail" (an MCP tool step), "chart" (a rendered figure),
//               "costStatistics" (terminal usage summary), or "error".
//               Never NULL.
//
//               These are the SAME tags the HTTP/SSE transport sends, and they
//               come off the same chat-tier filter pipeline, so an embedded
//               client and a network client see one event vocabulary.
//   content     the UTF-8 text payload, NUL-terminated. Never NULL but may be
//               empty (e.g. for "costStatistics"). Only valid for the duration
//               of the call — copy it if you need to keep it.
//
//               For "chart" this is the Apache ECharts *option* as a JSON
//               object string, ready to parse and hand to setOption(). It is
//               deliberately NOT delivered as text inside the reply: an option
//               inlined into the answer would be replayed to the model as
//               context on every later turn, and land in the clipboard when the
//               user copies the reply.
//
//               A "chart" event REPLACES the render_chart tool's
//               queryTitle/queryArguments/queryDetail triple, which is
//               suppressed — so a handler that draws charts needs no tool-name
//               sniffing, and one that ignores "chart" simply shows no figure.
//   user_data   the opaque pointer passed to mirobody_chat, forwarded verbatim.
//
// Return non-zero to keep streaming, 0 to abort the turn early.
typedef int (*mirobody_chat_handler)(
    const char* event_type,
    const char* content,
    void* user_data);

// Run one chat turn against an LLM with the MCP tools wired in, streaming the
// response through `on_event`. Blocks until the turn completes.
//
// Uses the process-wide config (loaded automatically on first use from
// MIROBODY_CONFIG / ./config.yml / built-in defaults), so it works whether or
// not mirobody_start() has been called — no server need be running. The LLM
// provider credentials come from that config (e.g. OPENAI_API_KEY).
//
//   provider  a token from mirobody_get_providers — pass one straight through.
//             Either an "agent/model" pair (part before the first '/' is the
//             agent, after it the model) or a bare model for the default agent
//             (e.g. "gpt-5-nano"). A bare token that names no registered agent is
//             taken as a model and routed to the default agent; an empty model =>
//             the agent's default model. NULL or empty => the default agent and
//             its default model.
//   message   the user's message. Required; NULL or empty returns an error.
//   user_id   caller's row id, so auth-scoped MCP tools run as that user; pass
//             0 for an anonymous turn.
//   on_event  streaming callback (see above). Required.
//   user_data opaque pointer forwarded to every on_event call. May be NULL.
//
// Returns 0 on success, non-zero on failure (missing message/callback, unknown
// agent, or the handler aborted the stream). Provider-level errors are NOT a
// non-zero return — they are delivered as an "error" event to on_event.
int mirobody_chat(
    const char* provider,
    const char* message,
    long long user_id,
    mirobody_chat_handler on_event,
    void* user_data);

// mirobody_chat for a host that manages its own conversation history: the same
// turn, but the context arrives as a JSON array of chat messages instead of a
// single string. This is the entry point an embedded client (the HarmonyOS
// bridge) calls -- the server-side session memory needs a cache + session id the
// C ABI does not carry, so multi-turn context must ride in with the request.
//
//   messages_json  '[{"role":"user"|"assistant"|"system","content":"..."}, ...]'
//                  in conversation order, ending with the current user turn.
//                  Unknown members are ignored; entries missing role/content are
//                  skipped. Required; empty/unparseable => error return.
//
// Everything else (provider token, user_id, callback protocol, return values)
// is exactly mirobody_chat.
int mirobody_chat_messages(
    const char* provider,
    const char* messages_json,
    long long user_id,
    mirobody_chat_handler on_event,
    void* user_data);

// Answer an "ask" event, so a turn parked on the `ask_user` tool can continue.
//
// The turn is BLOCKED inside the tool call when this event arrives — the model
// asked and is waiting for the reply, rather than ending its turn — so the answer
// must come from a DIFFERENT thread than the one running mirobody_chat_messages.
// Nothing is in flight to the model while it waits: a round of tool calling is a
// completed request, and the next round is a new one, so the user may take as long
// as they like. What is held is one thread here.
//
//   ask_id       the "ask_id" from the event's JSON payload, verbatim.
//   answer_json  the user's choice, as a JSON object. The shape is the caller's,
//                and reaches the model as the tool's result — the built-in clients
//                send {"selected":["..."],"other":"free text"}.
//
// Returns 1 when a waiting turn took it, 0 when none was waiting. A stale answer
// (the turn was stopped, the tool timed out, the user tapped twice) is DROPPED,
// never queued for whatever asks next. Answering is idempotent in the sense that
// only the first call wins; later ones return 0.
//
// A client that never answers is not an error: the tool times out on its own and
// tells the model nobody replied, so the turn always finishes.
int mirobody_chat_answer(const char* ask_id, const char* answer_json);

// Abandon every turn waiting on an answer. Their tools return "no answer" and the
// turns finish normally. Call it when tearing down, or when the user leaves a
// screen where answering is no longer possible.
void mirobody_chat_answer_cancel_all(void);

//------------------------------------------------------------------------------
// Files
//------------------------------------------------------------------------------
//
// A serverless file API for hosts (e.g. a Go/Java wrapper exposing its own HTTP
// API) that store and serve a user's uploads through this library rather than
// the built-in HTTP front door. They use the process-wide config (object store,
// cache, and FILE_ENCRYPTION_KEY / FILE_KEY_SEED) loaded automatically on first
// use, so they work whether or not mirobody_start() is running, and store /
// read files compatibly with the server (same keys, same encryption). All
// require an object store to be configured; they return NULL / -1 otherwise.
// `user_id` scopes every call to one user (its uploads live under a hashed
// prefix); pass the caller's row id.

// Store one file for `user_id` and return its object key (the handle for
// mirobody_read_file and the `file_key` mirobody_list_files reports). `data` /
// `len` are the raw bytes (encrypted at rest when FILE_ENCRYPTION_KEY is set);
// `filename` and `mime_type` may be NULL (the type is then inferred from the
// filename extension, else "application/octet-stream"). The returned buffer is
// owned by mirobody and valid until the next file call on the same thread --
// copy it. Returns NULL on error. (Text extraction is not performed here; a
// file's parsed text appears only once extracted via the chat upload path.)
const char* mirobody_store_file(
    long long user_id,
    const char* filename,
    const char* mime_type,
    const void* data,
    size_t len);

// The caller's uploads as a JSON object, newest first and paginated:
//   {"files":[{"filename","mime_type","file_key","text_key","summary",
//              "uploaded_at"}...], "total":<int>,"page":<int>,"size":<int>}
// Served from the `files` table when a database is available (full pagination
// and total), else from the cache/sidecar index (newest-first within its cap).
// `text_key` is "" when no extracted text exists; `summary` is "" until
// post-upload processing fills it. Read bytes / text with mirobody_read_file
// (file_key + want_text). `page` (>=0) / `size` (1..100, default 20) page the
// list. The buffer is owned by mirobody and valid until the next file call on
// the same thread -- copy it. Returns NULL on error.
const char* mirobody_list_files(long long user_id, int page, int size);

// Read one of `user_id`'s files by `key` (ownership-checked: a key outside the
// caller's namespace returns NULL). With `want_text` == 0 returns the raw bytes;
// non-zero returns the extracted text (empty, length 0, when the file has none).
// Writes the byte length to *out_len (may be NULL). The returned pointer is owned
// by mirobody and valid until the next file call on the same thread -- copy it.
// Returns NULL when not found / not owned, or on error.
const void* mirobody_read_file(
    long long user_id,
    const char* key,
    int want_text,
    size_t* out_len);

//------------------------------------------------------------------------------
// Health data (on-device ingest)
//------------------------------------------------------------------------------
//
// A serverless FHIR write + read-back path for a host that reads the platform's
// own health store and has nowhere to POST it: the HarmonyOS client, whose whole
// premise is that no Mirobody server exists (harmony/README.md). Where the
// Android app reads Health Connect / Huawei Health Kit and POSTs Observations to
// the server's /fhir endpoint, this stores them in the SAME fhir_resources table
// on the device, so the `family_health` MCP tool — which the embedded build runs
// for every turn — can answer questions about the user's own data with nothing
// leaving the phone.
//
// Both calls need the database the embedded host configured (SQLITE_PATH +
// SQL_DIR via mirobody_set_config, exactly as the chat tools do) and apply the
// DDL on first use. `user_id` <= 0 means the device owner (row 1) in the mobile
// build, matching mirobody_chat's anonymous turns.
//
// Callable from any thread: the two share one connection of their own behind one
// lock, rather than the chat services' (database::Database is not thread-safe).
// It is the same database FILE, so a turn's family_health tool sees what a sync
// just wrote. Under SQLite's WAL that read never blocks; a sync landing exactly
// during a turn's own write can still lose a write-write race, reported as this
// call's "error" instead of being retried silently.

// Store a JSON array of FHIR resources (Observations, from a platform health
// store) for `user_id`. Returns a JSON summary, or NULL on a hard failure (no
// database, unparseable argument):
//   {"stored":<int>,"failed":<int>,"error":"<first failure, or empty>"}
//
// IDEMPOTENT BY CLIENT-SUPPLIED ID, and that is the point: give each reading a
// deterministic `id` derived from its (metric, instant) — the HarmonyOS client
// uses "hw.<metric>.<startMillis>" — and re-syncing an overlapping window
// REPLACES those readings instead of duplicating them. A resource with no id
// gets a fresh server-assigned one, i.e. plain POST semantics, so a caller that
// has no stable key still works (it just cannot re-sync cleanly).
//
// An entry that fails validation is counted in "failed" and skipped; one bad
// reading never aborts the batch. The buffer is owned by mirobody and valid
// until this thread's next health call — copy it.
const char* mirobody_health_store(long long user_id, const char* resources_json);

// The caller's most recent Observations, flattened for display:
//   {"total":<int>,"items":[{"code","display","value","unit","time","source"}...]}
// `code` is the LOINC code, `time` the effective instant (or period start), and
// `value`/`unit` come from valueQuantity; a resource carrying none of that is
// listed with empty fields rather than dropped. `total` counts every live
// Observation, not just the page. `count` is clamped to 1..200 (default 20).
//
// Two orderings, on purpose: the newest `count` rows by WRITE time are selected
// (that is the store's index), then the page is returned sorted by READING time,
// newest first. Without the second step a batch written in one sync -- all sharing
// an instant -- would come back grouped by resource id, i.e. by metric name.
//
// Same buffer ownership as above. NULL on error.
const char* mirobody_health_recent(long long user_id, int count);

//------------------------------------------------------------------------------
// On-device LLM (llama.cpp + a GGUF file)
//------------------------------------------------------------------------------
//
// A C door onto llm::LocalClient, for hosts that cannot hold a C++ object: iOS
// reaches it from Swift through the bridging header, exactly as Android reaches
// the same class through JNI. Deliberately the SAME shape as those JNI entry
// points (open / load / generate / cancel / close), because the two are two
// spellings of one contract and a difference between them would be a bug waiting
// to happen rather than a design.
//
// Nothing here talks to a server, a provider, or the config. It is the offline
// lane, and it is available even in a build with no server started.
//
// Handle-based because the weights must NOT be re-read between turns: that is
// seconds of work over a few GB. One handle owns one model for its life and
// builds a fresh context per turn.

// Opaque on-device engine handle.
typedef struct mirobody_llm mirobody_llm_t;

// Event kinds for mirobody_llm_handler. Reasoning arrives as its own kind rather
// than inline, because a reasoning model writes `<think>…</think>` into the token
// stream and the split happens natively — the caller never sees a tag.
#define MIROBODY_LLM_REPLY    0
#define MIROBODY_LLM_THINKING 1
#define MIROBODY_LLM_ERROR    2

// Non-zero when this build linked llama.cpp. Zero means every call below is inert
// (open returns NULL), which is the graceful-degradation contract the rest of this
// header follows: a missing native piece never crashes a host, it just says no.
int mirobody_llm_available(void);

// Where to look for dlopen-able ggml backend modules — Android's
// GGML_CPU_ALL_VARIANTS build, where the CPU kernels ship as one .so per feature
// level and the best for this chip is chosen at startup. Call once, before
// anything else here.
//
// A no-op in a statically linked build, which is every Apple one: iOS cannot
// dlopen code the app wrote, so there is nothing to choose and the single -march
// the toolchain settles on is the whole answer.
void mirobody_llm_backend_path(const char* dir);

// Open an engine over the GGUF at `model_path`. Cheap — nothing is read yet.
//
//   threads   worker threads; <= 0 takes the library's own tuned default, which
//             is NOT hardware_concurrency (see LocalOptions::n_threads).
//   thinking  1 to reason before answering, 0 not to, -1 to leave it to the
//             model. Explicit because otherwise it is an accident: the built-in
//             chat template has no Jinja engine, so a model's own
//             `enable_thinking` conditional never fires.
//
// Returns NULL when llama.cpp is not linked in or allocation fails.
mirobody_llm_t* mirobody_llm_open(const char* model_path, int threads, int thinking);

// Free the engine and its weights. Safe on NULL.
void mirobody_llm_close(mirobody_llm_t* handle);

// Ask the in-flight turn to stop. Safe from another thread — that is the whole
// point of it — and a no-op when idle.
void mirobody_llm_cancel(mirobody_llm_t* handle);

// Read the weights now, so the first turn does not pay for them. BLOCKS for
// seconds over a few GB; idempotent.
//
// Returns "" on success, else why not. Same buffer ownership as the health calls:
// owned by mirobody, valid until this thread's next mirobody_llm_load.
const char* mirobody_llm_load(mirobody_llm_t* handle);

// Non-zero once the weights are in memory, so a caller knows whether this turn
// owes the user a spinner.
int mirobody_llm_loaded(mirobody_llm_t* handle);

// Streaming callback for mirobody_llm_generate. `kind` is one of the
// MIROBODY_LLM_* constants; `text` is UTF-8, NUL-terminated, valid only for the
// duration of the call. Return non-zero to keep streaming, 0 to stop the turn at
// the next token — which is how a cancelled UI ends a decode loop that would
// otherwise run for minutes.
typedef int (*mirobody_llm_handler)(int kind, const char* text, void* user_data);

// Run one turn, streaming into `on_event`. BLOCKS until the reply ends or is
// stopped, so call it off whatever thread draws.
//
// `roles` and `contents` are parallel arrays of `count` strings: roles[i] is
// "user" or "assistant" for contents[i]. The WHOLE transcript, every turn — the
// library owns the window and drops the oldest messages until the prompt fits,
// keeping a reserve free for the answer, so trimming here too would only make two
// policies disagree.
//
// `system_prompt` may be NULL or empty. Returns non-zero on success; a provider-
// level failure is delivered as a MIROBODY_LLM_ERROR event, not as a return code.
int mirobody_llm_generate(
    mirobody_llm_t* handle,
    const char* const* roles,
    const char* const* contents,
    int count,
    const char* system_prompt,
    mirobody_llm_handler on_event,
    void* user_data);

#ifdef __cplusplus
}
#endif

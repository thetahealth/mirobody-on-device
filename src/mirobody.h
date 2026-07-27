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
//               "queryDetail" (an MCP tool step), "costStatistics" (terminal
//               usage summary), or "error". Never NULL.
//   content     the UTF-8 text payload, NUL-terminated. Never NULL but may be
//               empty (e.g. for "costStatistics"). Only valid for the duration
//               of the call — copy it if you need to keep it.
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

#ifdef __cplusplus
}
#endif

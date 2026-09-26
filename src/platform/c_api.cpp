// Generic C-ABI implementation of the public surface in mirobody.h.
//
// This is the "host shim" for every FFI consumer that speaks the C ABI — Java
// (Panama/JNA/JNI), Go (cgo), C# (P/Invoke), Rust, Node, Ruby, … They all link
// against the one shared library this file is compiled into (libmirobody, the
// mirobody_shared target in CMakeLists.txt) and call the C functions below: the
// server lifecycle (mirobody_start / _stop / _is_running / _listen_port) plus
// mirobody_chat, a serverless one-shot LLM+MCP turn driven off the global config.
//
// It sits beside the platform-specific bridges that already implement the same
// contract their own way: ios_bridge.mm (iOS, same C API) and android_jni.cpp
// (Android, JNI-mangled symbols). Those are compiled only into their respective
// mobile targets, so there is no duplicate-symbol clash with this file — it is
// only ever built into the desktop/server shared library.
//
// Unlike the iOS bridge, this applies the database schema before serving (like
// main.cpp), so a host that just dlopen()s the library and calls mirobody_start
// gets a fully migrated database with no extra steps.

#if defined(__APPLE__)
#include <TargetConditionals.h>
#endif

// Compiled into the desktop/server shared library only. Excluded on Android
// (JNI symbols, android_jni.cpp) and iOS (same C API, ios_bridge.mm) so the
// platform that owns the surface there provides the single definition.
#if !defined(__ANDROID__) && !(defined(__APPLE__) && TARGET_OS_IPHONE)

#include "mirobody.h"

#include "cache/cache.hpp"
#include "chat/agent.hpp"
#include "chat/event/event.hpp"
#include "chat/event/filter/filter.hpp"
#include "client/http_client.hpp"
#include "config/config.hpp"
#include "database/database.hpp"
#include "database/schema.hpp"
#include "fhir/resource.hpp"
#include "fhir/store.hpp"
#include "fhir/write.hpp"
#include "llm/event.hpp"
#include "mcp/ask.hpp"
#include "memory/memory.hpp"
#include "platform/c_api_json.hpp"
#include "platform/log.hpp"
#include "storage/storage.hpp"
#include "transcode/file.hpp"

// The mobile/embedded profile has no HTTP front door (MIROBODY_MOBILE in
// CMakeLists.txt), so there is no Server to start: the four lifecycle functions
// below become not-running stubs and everything else -- chat, files -- is
// unaffected. That is how the HarmonyOS bridge under harmony/entry/src/main/cpp
// reaches the core: mirobody_chat only.
#ifndef MIROBODY_MOBILE
#  define MIROBODY_MOBILE 0
#endif

#if !MIROBODY_MOBILE
#  include "server/server.hpp"
#  include <libwebsockets.h>
#endif

#include <rapidjson/document.h>
#include <rapidjson/stringbuffer.h>
#include <rapidjson/writer.h>

#include <algorithm>   // sort (mirobody_health_recent orders by reading time)
#include <cstddef>
#include <cstdint>
#include <cstdlib>   // setenv / _putenv_s (mirobody_set_config)
#include <exception>
#include <memory>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

namespace {

std::once_flag g_curl_init_flag;

void ensure_curl_init() {
    std::call_once(g_curl_init_flag, [] {
        mirobody::client::HttpClient::global_init();
    });
}

#if !MIROBODY_MOBILE
void lws_log_to_platform(int /*level*/, const char* line) {
    mirobody::platform::log_info("%s", line);
}
#endif

std::string c_to_std(const char* s) {
    return s ? std::string(s) : std::string{};
}

// Apply the DDL on a throwaway connection before the server opens its serving
// pool — mirrors main.cpp. The branch is fixed at build time by the selected
// database backend (see CMakeLists.txt); the #error guards new backends.
void apply_migrations(const mirobody::Config& cfg) {
    mirobody::database::Database db(cfg.sqlite.open());
    mirobody::database::apply_schema(db, cfg.sql_dir + "/" MIROBODY_DATABASE_BACKEND_DIR);
}

// Build every agent's provider clients from the global config, exactly once
// across all mirobody_chat calls. load_clients() is itself idempotent; the flag
// just avoids rebuilding the client store (and re-reading config) on every turn.
void ensure_agent_clients() {
    static std::once_flag flag;
    std::call_once(flag, [] {
        mirobody::chat::agent_registry().load_clients(mirobody::config());
    });
}

// Serverless file backend: a Cache + Storage built once from the global config,
// for the file C functions (store / list / read) so a host can use them without
// standing up the HTTP server. Mirrors how the server wires these -- same
// backend precedence (S3 > OSS > local), same cache (Redis when configured else
// in-memory), and crucially the SAME encryption setup
// (storage::configure_file_encryption), so files stored here decrypt under the
// server and vice versa. The cache may differ from a separately-running server's
// in-memory cache, but the .meta sidecars are the durable source of truth, so a
// cold cache just rebuilds -- never wrong.
struct FileContext {
    std::unique_ptr<mirobody::cache::Cache>     cache;
    std::unique_ptr<mirobody::storage::Storage> storage;
    std::unique_ptr<mirobody::database::Database> db;   // null => list/store fall back to the cache index
    bool ok = false;
};

// Open a Database connection for the linked backend, mirroring apply_migrations
// (the backend is fixed at build time). Returns null on failure; the file API
// then degrades to the cache/sidecar index (no `files`-table queries).
std::unique_ptr<mirobody::database::Database> open_file_db(const mirobody::Config& cfg) {
    try {
        return std::unique_ptr<mirobody::database::Database>(
            new mirobody::database::Database(cfg.sqlite.open()));
    } catch (const std::exception& e) {
        mirobody::platform::log_warn("mirobody file api: database unavailable (%s); "
                                     "list/store fall back to the cache index", e.what());
        return nullptr;
    }
}

FileContext& file_context() {
    static FileContext ctx;
    static std::once_flag flag;
    std::call_once(flag, [] {
        ensure_curl_init();
        try {
            const mirobody::Config& cfg = mirobody::config();
            if (cfg.local_storage().configured()) ctx.storage = cfg.local_storage().open();
            if (!ctx.storage) {
                mirobody::platform::log_error("mirobody file api: no object storage configured");
                return;
            }
            ctx.cache.reset(new mirobody::cache::Cache(cfg.memory_kv.open()));
            ctx.db = open_file_db(cfg);   // best-effort; assumed already migrated

            if (!cfg.file_encryption_keys.empty()) {
                const mirobody::storage::FileEncryptionResult fe =
                    mirobody::storage::configure_file_encryption(
                        *ctx.storage, cfg.file_encryption_keys, cfg.file_key_seed,
                        cfg.file_url_prefix, cfg.file_url_base);
                if (!fe.ok) {
                    mirobody::platform::log_error("mirobody file api: %s", fe.error.c_str());
                    ctx.storage.reset();
                    ctx.cache.reset();
                    return;
                }
            }
            ctx.ok = true;
        } catch (const std::exception& e) {
            mirobody::platform::log_error("mirobody file api: init failed: %s", e.what());
        }
    });
    return ctx;
}

}  // namespace

//------------------------------------------------------------------------------

#if MIROBODY_MOBILE

// Built without the HTTP front door: nothing to start. Hosts are expected to
// call mirobody_chat / the file functions directly, which need no server (they
// load the process-wide config on first use). Returning NULL is the documented
// failure mode, and matches how a pure-client build behaves on iOS/Android.

extern "C" mirobody_server_t* mirobody_start(const char*, const char*) {
    mirobody::platform::log_info(
        "mirobody_start: built without the HTTP front door; use mirobody_chat directly");
    return nullptr;
}

extern "C" void mirobody_stop(mirobody_server_t*)          {}
extern "C" int  mirobody_is_running(mirobody_server_t*)    { return 0; }
extern "C" int  mirobody_listen_port(mirobody_server_t*)   { return -1; }

#else

extern "C" mirobody_server_t* mirobody_start(
        const char* config_path,
        const char* data_dir) {

    ensure_curl_init();

    int log_mask = LLL_ERR | LLL_WARN;
    lws_set_log_level(log_mask, lws_log_to_platform);

    std::string cfg_path = c_to_std(config_path);
    std::string data     = c_to_std(data_dir);

    mirobody::Config cfg;
    try {
        if (!cfg_path.empty()) {
            cfg = mirobody::load_config(cfg_path);
        } else {
            cfg = mirobody::load_config();
        }
    } catch (const std::exception& e) {
        mirobody::platform::log_error("config error: %s", e.what());
        return nullptr;
    }

    // LLM keys and the listen port come from the config (and its env-var
    // fallbacks: OPENAI_API_KEY / GOOGLE_API_KEY / HTTP_PORT).
    // An embedding host reaches the core over loopback; the compiled 0.0.0.0
    // default would expose it on every interface (see android_jni.cpp).
    if (cfg.listen_addr.empty() || cfg.listen_addr == "0.0.0.0") {
        cfg.listen_addr = "127.0.0.1";
    }
    // data_dir, when given, is where the on-device SQLite file lives.
    if (!data.empty())       cfg.sqlite.path = data + "/mirobody.db";

    try {
        apply_migrations(cfg);
    } catch (const std::exception& e) {
        mirobody::platform::log_error("database init failed: %s", e.what());
        return nullptr;
    }

    auto server = std::unique_ptr<mirobody::Server>(new mirobody::Server(std::move(cfg)));
    if (!server->start_in_background()) {
        mirobody::platform::log_error("server failed to start");
        return nullptr;
    }
    return reinterpret_cast<mirobody_server_t*>(server.release());
}

//------------------------------------------------------------------------------

extern "C" void mirobody_stop(mirobody_server_t* handle) {
    if (!handle) return;
    auto* server = reinterpret_cast<mirobody::Server*>(handle);
    server->stop_and_join();
    delete server;
}

//------------------------------------------------------------------------------

extern "C" int mirobody_is_running(mirobody_server_t* handle) {
    if (!handle) return 0;
    auto* server = reinterpret_cast<mirobody::Server*>(handle);
    return server->is_running() ? 1 : 0;
}

//------------------------------------------------------------------------------

extern "C" int mirobody_listen_port(mirobody_server_t* handle) {
    if (!handle) return -1;
    auto* server = reinterpret_cast<mirobody::Server*>(handle);
    return static_cast<int>(server->config().listen_port);
}

#endif   // MIROBODY_MOBILE

//------------------------------------------------------------------------------

extern "C" const char* mirobody_get_providers(void) {
    // Owned by the library and returned to the caller: thread_local so the
    // pointer is stable until this thread calls again, with no cross-thread race.
    static thread_local std::string result;
    result.clear();

    ensure_curl_init();

    // Loads the global config and builds each agent's clients on first use, so
    // the set we report matches what mirobody_chat can actually run.
    try {
        ensure_agent_clients();
    } catch (const std::exception& e) {
        mirobody::platform::log_error("mirobody_get_providers: client init failed: %s", e.what());
        return nullptr;
    }

    const std::vector<std::string> names =
        mirobody::chat::agent_registry().provider_names(/*public_only=*/true);

    // One "Agent/model" pair per line.
    for (std::size_t i = 0; i < names.size(); ++i) {
        if (i) result.push_back('\n');
        result += names[i];
    }
    return result.c_str();
}

//------------------------------------------------------------------------------

//------------------------------------------------------------------------------
// Configuration (embedded hosts)
//------------------------------------------------------------------------------

extern "C" int mirobody_set_config(const char* key, const char* value) {
    if (!key || !*key) return -1;
#ifdef _WIN32
    // _putenv_s with "" removes the variable, which is exactly the NULL contract.
    return _putenv_s(key, value ? value : "") == 0 ? 0 : -1;
#else
    if (!value) return unsetenv(key) == 0 ? 0 : -1;
    return setenv(key, value, /*overwrite=*/1) == 0 ? 0 : -1;
#endif
}

extern "C" int mirobody_reload_providers(void) {
    ensure_curl_init();
    try {
        // init_config re-reads every source, environment included -- the typed
        // Config fields (cfg.zhipu.api_key, ...) snapshot the environment at
        // load time, so a plain load_clients() over the OLD config would not see
        // anything mirobody_set_config changed.
        mirobody::Config& cfg = mirobody::init_config();
        mirobody::chat::agent_registry().load_clients(cfg);
        return static_cast<int>(
            mirobody::chat::agent_registry().provider_names(/*public_only=*/true).size());
    } catch (const std::exception& e) {
        mirobody::platform::log_error("mirobody_reload_providers: %s", e.what());
        return -1;
    }
}

//------------------------------------------------------------------------------
// Chat
//------------------------------------------------------------------------------

namespace {

// Serverless chat services: the database / cache / long-term memory the
// auth-scoped MCP tools (family_health, remember, recall_memory, the history
// tools) reach through the AgentRequest. Built once, lazily, from the global
// config -- mirroring how the server wires them -- and each piece degrades
// independently: no database => db and memory stay null and the tools that
// need them answer with their own "unavailable" errors, while the turn itself
// runs fine. The embedded host must inject SQLITE_PATH and SQL_DIR (see
// mirobody_set_config) before the first chat for the database to come up.
struct ChatServices {
    std::unique_ptr<mirobody::database::Database> db;
    std::unique_ptr<mirobody::cache::Cache>       cache;
    std::unique_ptr<mirobody::memory::Memory>     memory;
};

ChatServices& chat_services() {
    static ChatServices ctx;
    static std::once_flag flag;
    std::call_once(flag, [] {
        const mirobody::Config& cfg = mirobody::config();
        try {
            apply_migrations(cfg);        // idempotent DDL, same as mirobody_start
            ctx.db = open_file_db(cfg);   // backend-appropriate connection (null on failure)
        } catch (const std::exception& e) {
            mirobody::platform::log_warn(
                "mirobody chat: database unavailable (%s); db-backed tools degrade", e.what());
        }
        try {
            ctx.cache.reset(new mirobody::cache::Cache(cfg.memory_kv.open()));
        } catch (const std::exception& e) {
            mirobody::platform::log_warn("mirobody chat: cache unavailable (%s)", e.what());
        }
        if (ctx.db) {
            try {
                ctx.memory = mirobody::memory::make_memory(cfg, *ctx.db);
            } catch (const std::exception& e) {
                mirobody::platform::log_warn(
                    "mirobody chat: memory store unavailable (%s)", e.what());
            }
        }
    });
    return ctx;
}

// Shared body of mirobody_chat / mirobody_chat_messages: everything after the
// conversation itself has been assembled. `question` is the current user turn
// (persist_history's summary field); `messages` the full context in order.
int run_chat_turn(const char* provider,
                  std::vector<mirobody::llm::ChatMessage> messages,
                  std::string question,
                  long long user_id,
                  mirobody_chat_handler on_event,
                  void* user_data) {

    ensure_curl_init();

#if MIROBODY_MOBILE
    // Single-user device: the phone owner IS the first users row (the mobile
    // schema holds only them -- see res/sql/sqlite/1_user.sql). An anonymous
    // turn therefore runs as that user, so the auth-scoped tools (memory,
    // files, history, family_health) work instead of answering
    // "Authentication required".
    if (user_id <= 0) user_id = 1;
#endif

    // The global config is loaded automatically here on first use, so the host
    // can call mirobody_chat() without first standing up a server.
    try {
        ensure_agent_clients();
    } catch (const std::exception& e) {
        mirobody::platform::log_error("mirobody_chat: client init failed: %s", e.what());
        return -2;
    }

    // Split the "Agent/model" pair (as returned by mirobody_get_providers) into
    // the agent name and the model. No '/' => the whole token is one field (an
    // agent name, or -- with the prefix-less list -- a bare model). resolve_agent
    // below sorts that out.
    const mirobody::platform::ProviderToken token =
        mirobody::platform::split_provider(c_to_std(provider));
    std::string agent_name = token.agent;

    mirobody::chat::AgentRequest req;
    req.question = std::move(question);
    req.messages = std::move(messages);
    req.provider = token.model;
    req.user_id  = static_cast<std::int64_t>(user_id);

    // Hand the locally-executed tools their services (see ChatServices above).
    // The object store rides on the file API's context so files stored through
    // either surface share one backend; only consulted when one is configured,
    // so a chat-only setup doesn't pay for (or log about) storage.
    {
        ChatServices& svc = chat_services();
        req.cache  = svc.cache.get();
        req.db     = svc.db.get();
        req.memory = svc.memory.get();

        const mirobody::Config& cfg = mirobody::config();
        if (cfg.local_storage().configured()) {
            FileContext& fc = file_context();
            if (fc.ok) req.storage = fc.storage.get();
        }
    }

    // A token naming no agent is a bare provider under the default agent (the
    // prefix-less list); an empty token likewise defaults. resolve_agent carries
    // the bare token into req.provider when no model was split out.
    agent_name = mirobody::chat::agent_registry().resolve_agent(agent_name, req.provider);
    if (agent_name.empty()) {
        mirobody::platform::log_error("mirobody_chat: no agents registered");
        return -3;
    }

    std::unique_ptr<mirobody::chat::Agent> instance =
        mirobody::chat::agent_registry().create(agent_name, req);
    if (!instance) {
        mirobody::platform::log_error("mirobody_chat: no such agent '%s'", agent_name.c_str());
        return -3;
    }

    // Adapt the streamed llm events onto the C callback. Returning false from
    // the handler aborts the in-flight turn; we propagate the host's 0 return as
    // that abort signal and remember it so we can report it to the caller.
    //
    // The llm stream goes through the CHAT tier's filter pipeline on the way out,
    // exactly as the server's dispatcher routes it. This used to hand llm::Events
    // straight to the callback, which meant every filter was server-only: an
    // embedded client saw the raw render_chart tool triple where an HTTP client
    // saw one clean "chart" event, and had to re-implement ChartFilter itself to
    // catch up. One pipeline, one vocabulary, and a filter added to
    // make_event_pipeline() now reaches both transports by construction.
    bool aborted = false;
    const mirobody::chat::EventPipeline pipeline = mirobody::chat::make_event_pipeline();

    const mirobody::chat::EventFilter::Sink out =
        [&](const mirobody::chat::Event& ev) -> bool {
            // payload(), not to_json(): the C ABI carries a flat (type, content)
            // pair, so a chart arrives as its ECharts option ready to parse.
            const std::string body = ev.payload();
            const int keep = on_event(ev.type(), body.c_str(), user_data);
            if (keep == 0) { aborted = true; return false; }
            return true;
        };

    mirobody::llm::EventHandler sink =
        [&](const mirobody::llm::Event& e) -> bool {
            const std::unique_ptr<mirobody::chat::Event> ce = mirobody::chat::event_from_llm(e);
            // An llm event with no chat counterpart is dropped rather than
            // guessed at; event_from_llm covers every type today.
            if (!ce) return true;
            return pipeline.feed(*ce, out);
        };

    try {
        instance->generate_response(req, sink);
    } catch (const std::exception& e) {
        mirobody::platform::log_error("mirobody_chat: %s", e.what());
        return -4;
    }

    return aborted ? 1 : 0;
}

}   // namespace

extern "C" int mirobody_chat(
        const char* provider,
        const char* message,
        long long user_id,
        mirobody_chat_handler on_event,
        void* user_data) {

    if (!on_event) return -1;

    const std::string msg = c_to_std(message);
    if (msg.empty()) {
        mirobody::platform::log_error("mirobody_chat: empty message");
        return -1;
    }

    std::vector<mirobody::llm::ChatMessage> messages;
    messages.push_back(mirobody::llm::ChatMessage{"user", msg, {}});
    return run_chat_turn(provider, std::move(messages), msg, user_id, on_event, user_data);
}

extern "C" int mirobody_chat_messages(
        const char* provider,
        const char* messages_json,
        long long user_id,
        mirobody_chat_handler on_event,
        void* user_data) {

    if (!on_event) return -1;

    // Tolerant extraction: an entry missing role or content is skipped rather
    // than failing the turn. `question` is the LAST user turn -- the summary
    // persist_history records, and what history-keyed features key off.
    mirobody::platform::ChatInput in = mirobody::platform::parse_chat_messages(messages_json);
    if (in.error == mirobody::platform::ChatInputError::NotAnArray) {
        mirobody::platform::log_error("mirobody_chat_messages: messages_json is not a JSON array");
        return -1;
    }
    if (in.error == mirobody::platform::ChatInputError::NoUsableMessages) {
        mirobody::platform::log_error("mirobody_chat_messages: no usable messages");
        return -1;
    }

    return run_chat_turn(provider, std::move(in.messages), std::move(in.question),
                         user_id, on_event, user_data);
}

extern "C" int mirobody_chat_answer(const char* ask_id, const char* answer_json) {
    const std::string id = c_to_std(ask_id);
    if (id.empty()) return 0;
    return mirobody::mcp::AskBroker::instance().answer(id, c_to_std(answer_json)) ? 1 : 0;
}

extern "C" void mirobody_chat_answer_cancel_all(void) {
    mirobody::mcp::AskBroker::instance().cancel_all();
}

//------------------------------------------------------------------------------
// Files (serverless)
//------------------------------------------------------------------------------

extern "C" const char* mirobody_store_file(
        long long user_id,
        const char* filename,
        const char* mime_type,
        const void* data,
        size_t len) {

    static thread_local std::string result;
    result.clear();
    if (user_id <= 0 || (data == nullptr && len != 0)) return nullptr;

    FileContext& ctx = file_context();
    if (!ctx.ok) return nullptr;

    try {
        const std::string fname = c_to_std(filename);
        const std::string ctype =
            mirobody::storage::resolve_content_type(fname, c_to_std(mime_type));
        const std::string bytes(static_cast<const char*>(data ? data : ""), len);

        mirobody::storage::ObjectMeta meta;
        meta.filename = fname;
        const std::string key = ctx.storage->put_user_object(
            static_cast<std::int64_t>(user_id), bytes, ctype, meta);

        // Index it so mirobody_list_files / the HTTP list see it immediately:
        // the cache/sidecar index always, plus the `files` table when present
        // (so a wrapper's own SQL over `files` sees C-API uploads too).
        mirobody::file::FileRef ref;
        ref.filename  = fname;
        ref.mime_type = ctype;
        ref.file_key  = key;
        mirobody::file::record(*ctx.cache, static_cast<std::int64_t>(user_id), ref);
        if (ctx.db) {
            try {
                mirobody::file::db_upsert_file(*ctx.db, static_cast<std::int64_t>(user_id),
                                               key, fname, ctype, static_cast<std::int64_t>(bytes.size()));
            } catch (const std::exception& e) {
                mirobody::platform::log_warn("mirobody_store_file: files index insert failed: %s", e.what());
            }
        }

        result = key;
        return result.c_str();
    } catch (const std::exception& e) {
        mirobody::platform::log_error("mirobody_store_file: %s", e.what());
        return nullptr;
    }
}

//------------------------------------------------------------------------------

extern "C" const char* mirobody_list_files(long long user_id, int page, int size) {
    static thread_local std::string result;
    result.clear();

    FileContext& ctx = file_context();
    if (!ctx.ok) return nullptr;

    if (page < 0) page = 0;
    if (size <= 0 || size > 100) size = 20;

    try {
        rapidjson::Document d;
        d.SetObject();
        rapidjson::Document::AllocatorType& a = d.GetAllocator();
        rapidjson::Value files(rapidjson::kArrayType);

        // One emitter, two sources. file_key/text_key are the handles the caller
        // passes back to mirobody_read_file (want_text 0/1).
        auto emit = [&](const std::string& filename, const std::string& mime,
                        const std::string& file_key, const std::string& text_key,
                        const std::string& summary, std::int64_t uploaded_at_ms) {
            rapidjson::Value item(rapidjson::kObjectType);
            auto add = [&](const char* k, const std::string& v) {
                item.AddMember(rapidjson::StringRef(k),
                               rapidjson::Value(v.c_str(), static_cast<rapidjson::SizeType>(v.size()), a), a);
            };
            add("filename", filename);
            add("mime_type", mime);
            add("file_key", file_key);
            add("text_key", text_key);
            add("summary", summary);
            item.AddMember("uploaded_at", static_cast<int64_t>(uploaded_at_ms), a);  // unix milliseconds
            files.PushBack(item, a);
        };

        int total = 0;
        // Preferred: the `files` table -- full pagination + total, newest first.
        if (ctx.db && user_id > 0) {
            const std::vector<mirobody::file::FileRow> rows = mirobody::file::db_list_files(
                *ctx.db, static_cast<std::int64_t>(user_id), "created_at", /*descending=*/true, size, page * size);
            for (std::size_t i = 0; i < rows.size(); ++i) {
                const mirobody::file::FileRow& r = rows[i];
                emit(r.filename, r.mime_type, r.file_key, r.text_key, r.summary, r.created_at);  // created_at is unix ms
            }
            total = static_cast<int>(mirobody::file::db_count_files(*ctx.db, static_cast<std::int64_t>(user_id)));
        } else
        {
            // Fallback (no DB / legacy schema): the cache + sidecar index,
            // newest-first within its cap; total is the listed count.
            const std::vector<mirobody::file::FileRef> refs =
                (user_id > 0) ? mirobody::file::list(*ctx.cache, ctx.storage.get(),
                                                     static_cast<std::int64_t>(user_id))
                              : std::vector<mirobody::file::FileRef>();
            const std::size_t offset = static_cast<std::size_t>(page) * static_cast<std::size_t>(size);
            for (std::size_t i = offset; i < refs.size() && i < offset + static_cast<std::size_t>(size); ++i) {
                const mirobody::file::FileRef& f = refs[i];
                // The sidecar's uploaded_at is unix SECONDS; surface it as ms to
                // match the DB path's created_at (the .meta store is unchanged).
                emit(f.filename, f.mime_type, f.file_key, f.text_key, std::string(),
                     f.uploaded_at > 0 ? f.uploaded_at * 1000 : 0);
            }
            total = static_cast<int>(refs.size());
        }

        d.AddMember("files", files, a);
        d.AddMember("total", total, a);
        d.AddMember("page",  page, a);
        d.AddMember("size",  size, a);

        rapidjson::StringBuffer buf;
        rapidjson::Writer<rapidjson::StringBuffer> w(buf);
        d.Accept(w);
        result.assign(buf.GetString(), buf.GetSize());
        return result.c_str();
    } catch (const std::exception& e) {
        mirobody::platform::log_error("mirobody_list_files: %s", e.what());
        return nullptr;
    }
}

//------------------------------------------------------------------------------

extern "C" const void* mirobody_read_file(
        long long user_id,
        const char* key,
        int want_text,
        size_t* out_len) {

    static thread_local std::string result;
    result.clear();
    if (out_len) *out_len = 0;
    if (user_id <= 0 || key == nullptr) return nullptr;

    FileContext& ctx = file_context();
    if (!ctx.ok) return nullptr;

    try {
        // find() pins the key to the caller's own hashed prefix -- the ownership
        // check -- before any fetch, so one user can't read another's bytes.
        mirobody::file::FileRef ref;
        if (!mirobody::file::find(*ctx.cache, ctx.storage.get(),
                                  static_cast<std::int64_t>(user_id), c_to_std(key), ref)) {
            return nullptr;   // not found / not owned
        }

        std::string target;
        if (want_text) {
            if (ref.text_key.empty()) return result.data();   // no extracted text: empty (len 0)
            target = ref.text_key;
        } else {
            target = ref.file_key.empty() ? c_to_std(key) : ref.file_key;
        }

        try {
            result = ctx.storage->get_object_decrypted(target);
        } catch (const mirobody::storage::StorageError&) {
            // e.g. want_text but the .trans object is absent -> "no content".
            result.clear();
            return want_text ? result.data() : nullptr;
        }
        if (out_len) *out_len = result.size();
        return result.data();
    } catch (const std::exception& e) {
        mirobody::platform::log_error("mirobody_read_file: %s", e.what());
        return nullptr;
    }
}

//------------------------------------------------------------------------------
// Health data (on-device ingest)
//------------------------------------------------------------------------------

namespace {

// The health calls' OWN database connection, plus the mutex that serializes it.
//
// Same FILE as the chat services' connection (so a sync writes exactly what the
// family_health MCP tool reads back during a turn), but deliberately a SEPARATE
// handle: database::Database is documented as not thread-safe, and these calls
// run on the host's worker threads while a chat turn may be using its own
// connection on another. One connection per user, each touched under one lock, is
// the contract that header asks for.
//
// SQLite is opened in WAL mode, so a reader never blocks the writer; two
// simultaneous WRITERS (a sync landing mid-turn) can still collide and raise
// SQLITE_BUSY, which surfaces as this call's "error" rather than being retried
// behind the user's back -- a sync is a button, and pressing it again is cheap.
struct HealthContext {
    std::unique_ptr<mirobody::database::Database> db;
    std::mutex mu;
};

HealthContext& health_context() {
    static HealthContext ctx;
    static std::once_flag flag;
    std::call_once(flag, [] {
        try {
            const mirobody::Config& cfg = mirobody::config();
            apply_migrations(cfg);          // idempotent DDL; a sync may be the first write
            ctx.db = open_file_db(cfg);     // backend-appropriate connection (null on failure)
        } catch (const std::exception& e) {
            mirobody::platform::log_error("mirobody health: database unavailable (%s)", e.what());
        }
    });
    return ctx;
}

// Resolve the writing/reading subject the way run_chat_turn does, so a sync and
// a turn agree on whose records these are.
long long health_subject(long long user_id) {
#if MIROBODY_MOBILE
    if (user_id <= 0) return 1;   // single-user device: the owner is row 1
#endif
    return user_id;
}

void json_add(rapidjson::Value& obj, const char* key, const std::string& val,
              rapidjson::Document::AllocatorType& a) {
    obj.AddMember(rapidjson::StringRef(key),
                  rapidjson::Value(val.c_str(), static_cast<rapidjson::SizeType>(val.size()), a), a);
}

}  // namespace

extern "C" const char* mirobody_health_store(long long user_id, const char* resources_json) {
    static thread_local std::string result;
    result.clear();

    rapidjson::Document in;
    if (!resources_json || in.Parse(resources_json).HasParseError() || !in.IsArray()) {
        mirobody::platform::log_error("mirobody_health_store: resources_json is not a JSON array");
        return nullptr;
    }

    const long long subject = health_subject(user_id);
    if (subject <= 0) {
        mirobody::platform::log_error("mirobody_health_store: no subject (user_id %lld)", user_id);
        return nullptr;
    }

    HealthContext& hc = health_context();
    std::lock_guard<std::mutex> lock(hc.mu);
    if (!hc.db) {
        mirobody::platform::log_error(
            "mirobody_health_store: no database -- set SQLITE_PATH and SQL_DIR first");
        return nullptr;
    }

    int stored = 0, failed = 0;
    std::string first_error;
    try {
        mirobody::fhir::FhirStore store(*hc.db);
        for (rapidjson::SizeType i = 0; i < in.Size(); ++i) {
            // Each resource gets its own Document: write_resource injects id +
            // meta into it, which needs an allocator the input array's elements
            // do not own individually.
            rapidjson::Document one;
            one.CopyFrom(in[i], one.GetAllocator());

            std::string type, id;
            const std::vector<mirobody::fhir::ValidationIssue> issues =
                mirobody::fhir::validate_resource(one, &type, &id);
            if (!issues.empty()) {
                ++failed;
                if (first_error.empty()) first_error = issues[0].diagnostics;
                continue;
            }
            // A caller-supplied id makes the write idempotent; an absent one is
            // assigned, matching POST /fhir/{type}.
            mirobody::fhir::write_resource(store, static_cast<std::int64_t>(subject), type, id, one);
            ++stored;
        }
    } catch (const std::exception& e) {
        // A store-level failure (schema missing, disk full) is not per-resource:
        // report what landed plus the error rather than pretending it all did.
        mirobody::platform::log_error("mirobody_health_store: %s", e.what());
        if (first_error.empty()) first_error = e.what();
        failed += static_cast<int>(in.Size()) - stored - failed;
    }

    rapidjson::Document d;
    d.SetObject();
    rapidjson::Document::AllocatorType& a = d.GetAllocator();
    d.AddMember("stored", stored, a);
    d.AddMember("failed", failed, a);
    json_add(d, "error", first_error, a);

    rapidjson::StringBuffer buf;
    rapidjson::Writer<rapidjson::StringBuffer> w(buf);
    d.Accept(w);
    result.assign(buf.GetString(), buf.GetSize());
    mirobody::platform::log_info("mirobody_health_store: user %lld stored=%d failed=%d",
                                 subject, stored, failed);
    return result.c_str();
}

//------------------------------------------------------------------------------

extern "C" const char* mirobody_health_recent(long long user_id, int count) {
    static thread_local std::string result;
    result.clear();

    const long long subject = health_subject(user_id);
    if (subject <= 0) return nullptr;

    HealthContext& hc = health_context();
    std::lock_guard<std::mutex> lock(hc.mu);
    if (!hc.db) {
        mirobody::platform::log_error("mirobody_health_recent: no database configured");
        return nullptr;
    }

    if (count <= 0)   count = 20;
    if (count > 200)  count = 200;

    try {
        mirobody::fhir::FhirStore store(*hc.db);
        std::int64_t total = 0;
        const std::vector<mirobody::fhir::StoredResource> hits = store.search(
            static_cast<std::int64_t>(subject), "Observation", std::string(), count, 0, total);

        std::vector<mirobody::platform::HealthItem> items_out;
        items_out.reserve(hits.size());

        for (std::size_t i = 0; i < hits.size(); ++i) {
            // An unreadable row is skipped rather than failing the listing.
            mirobody::platform::HealthItem item;
            if (mirobody::platform::flatten_observation_json(
                    hits[i].content.c_str(), hits[i].content.size(), item)) {
                items_out.push_back(item);
            }
        }
        std::sort(items_out.begin(), items_out.end(), mirobody::platform::later_reading);

        rapidjson::Document d;
        d.SetObject();
        rapidjson::Document::AllocatorType& a = d.GetAllocator();
        rapidjson::Value items(rapidjson::kArrayType);
        for (std::size_t i = 0; i < items_out.size(); ++i) {
            const mirobody::platform::HealthItem& it = items_out[i];
            rapidjson::Value item(rapidjson::kObjectType);
            json_add(item, "code", it.code, a);
            json_add(item, "display", it.display, a);
            if (it.has_value) item.AddMember("value", it.value, a);
            json_add(item, "unit", it.unit, a);
            json_add(item, "time", it.when, a);
            json_add(item, "source", it.source, a);
            items.PushBack(item, a);
        }

        d.AddMember("total", static_cast<int64_t>(total), a);
        d.AddMember("items", items, a);

        rapidjson::StringBuffer buf;
        rapidjson::Writer<rapidjson::StringBuffer> w(buf);
        d.Accept(w);
        result.assign(buf.GetString(), buf.GetSize());
        return result.c_str();
    } catch (const std::exception& e) {
        mirobody::platform::log_error("mirobody_health_recent: %s", e.what());
        return nullptr;
    }
}

#endif  // !__ANDROID__

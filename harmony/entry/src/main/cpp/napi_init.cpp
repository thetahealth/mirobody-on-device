// NAPI bridge: ArkTS <-> the mirobody C ABI (src/mirobody.h).
//
// The threading shape was proven by the step-0 probe before mirobody_core was
// linked: mirobody_chat_messages BLOCKS until the turn completes and invokes its
// handler once per event on the calling thread, so each turn runs on a detached
// worker thread and every event crosses back to ArkTS through one
// napi_threadsafe_function. Events arrive in order; the worker appends a
// synthetic terminal event ("end" on success, "aborted" after a cancel) since
// the C ABI signals completion by returning, not by a sentinel event.
//
// Cancellation: nativeChatCancel(turnId) flips a per-turn atomic; the chat
// handler checks it on the next event and returns 0, which aborts the in-flight
// HTTP transfer inside the core. A turn that produces no further events cannot
// be interrupted earlier -- same contract as the Canceler on the BYOK lane.

#include <napi/native_api.h>
#include <hilog/log.h>

#include "mirobody.h"

#include <atomic>
#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

#undef LOG_DOMAIN
#undef LOG_TAG
#define LOG_DOMAIN 0x3200
#define LOG_TAG "mirobody-napi"

namespace {

//------------------------------------------------------------------------------
// Helpers
//------------------------------------------------------------------------------

// Read a JS string argument into a std::string ('' on failure).
std::string ToUtf8(napi_env env, napi_value value) {
    size_t len = 0;
    if (napi_get_value_string_utf8(env, value, nullptr, 0, &len) != napi_ok) return std::string();

    std::string out(len, '\0');
    if (napi_get_value_string_utf8(env, value, &out[0], len + 1, &len) != napi_ok) return std::string();
    return out;
}

//------------------------------------------------------------------------------
// In-flight turns
//------------------------------------------------------------------------------

// One live chat turn. Owned jointly by the worker thread and the cancel table
// (shared_ptr), so a cancel racing the turn's end never touches freed memory.
struct Turn {
    std::atomic<bool> cancelled{false};
};

std::mutex g_turns_mutex;
std::map<std::int32_t, std::shared_ptr<Turn>> g_turns;
std::int32_t g_next_turn_id = 1;

// One event crossing the thread boundary; CallJs takes ownership.
struct StreamEvent {
    std::string type;
    std::string content;
};

// Runs ON THE ArkTS THREAD, once per queued event.
void CallJs(napi_env env, napi_value js_callback, void* /*context*/, void* data) {
    std::unique_ptr<StreamEvent> ev(static_cast<StreamEvent*>(data));
    if (env == nullptr || js_callback == nullptr) return;   // env tearing down

    napi_value undefined = nullptr;
    napi_get_undefined(env, &undefined);

    napi_value argv[2] = {nullptr, nullptr};
    napi_create_string_utf8(env, ev->type.c_str(), NAPI_AUTO_LENGTH, &argv[0]);
    napi_create_string_utf8(env, ev->content.c_str(), NAPI_AUTO_LENGTH, &argv[1]);

    napi_call_function(env, undefined, js_callback, 2, argv, nullptr);
}

// The mirobody_chat_handler context: the tsfn to forward events through and the
// turn's cancel flag.
struct HandlerContext {
    napi_threadsafe_function tsfn;
    std::shared_ptr<Turn>    turn;
};

// Forwarded once per chat event on the worker thread. Returning 0 aborts.
int OnChatEvent(const char* event_type, const char* content, void* user_data) {
    HandlerContext* ctx = static_cast<HandlerContext*>(user_data);
    if (ctx->turn->cancelled.load(std::memory_order_relaxed)) return 0;

    StreamEvent* ev = new StreamEvent{event_type ? event_type : "",
                                      content ? content : ""};
    // Blocking => backpressure instead of dropped events if ArkTS falls behind.
    napi_call_threadsafe_function(ctx->tsfn, ev, napi_tsfn_blocking);
    return 1;
}

// The worker: run the blocking turn, emit the terminal event, release the tsfn.
void ChatWorker(std::int32_t turn_id, std::string provider, std::string messages_json,
                napi_threadsafe_function tsfn, std::shared_ptr<Turn> turn) {
    HandlerContext ctx{tsfn, turn};

    const int rc = mirobody_chat_messages(provider.c_str(), messages_json.c_str(),
                                          /*user_id=*/0, &OnChatEvent, &ctx);
    OH_LOG_INFO(LOG_APP, "chat turn %{public}d finished rc=%{public}d", turn_id, rc);

    // rc: 0 done, 1 handler aborted (our cancel), <0 setup error (already
    // surfaced as a log; give ArkTS something actionable anyway).
    StreamEvent* fin;
    if (rc == 0)      fin = new StreamEvent{"end", ""};
    else if (rc == 1) fin = new StreamEvent{"aborted", ""};
    else              fin = new StreamEvent{"error", "native chat setup failed (rc " + std::to_string(rc) + ")"};
    napi_call_threadsafe_function(tsfn, fin, napi_tsfn_blocking);

    napi_release_threadsafe_function(tsfn, napi_tsfn_release);

    std::lock_guard<std::mutex> lock(g_turns_mutex);
    g_turns.erase(turn_id);
}

//------------------------------------------------------------------------------
// Exported functions
//------------------------------------------------------------------------------

// nativeSetConfig(key: string, value: string): number  (0 = ok)
napi_value NativeSetConfig(napi_env env, napi_callback_info info) {
    size_t argc = 2;
    napi_value argv[2] = {nullptr, nullptr};
    napi_get_cb_info(env, info, &argc, argv, nullptr, nullptr);

    const std::string key = argc > 0 ? ToUtf8(env, argv[0]) : std::string();
    const std::string val = argc > 1 ? ToUtf8(env, argv[1]) : std::string();

    const int rc = mirobody_set_config(key.c_str(), val.c_str());

    napi_value out = nullptr;
    napi_create_int32(env, rc, &out);
    return out;
}

// nativeReloadProviders(): number  (provider count, -1 on failure)
napi_value NativeReloadProviders(napi_env env, napi_callback_info /*info*/) {
    const int n = mirobody_reload_providers();
    napi_value out = nullptr;
    napi_create_int32(env, n, &out);
    return out;
}

// nativeGetProviders(): string  (newline-separated tokens; '' when none)
napi_value NativeGetProviders(napi_env env, napi_callback_info /*info*/) {
    const char* providers = mirobody_get_providers();
    if (providers == nullptr) {
        OH_LOG_ERROR(LOG_APP, "mirobody_get_providers failed (client init error)");
        providers = "";
    }
    napi_value out = nullptr;
    napi_create_string_utf8(env, providers, NAPI_AUTO_LENGTH, &out);
    return out;
}

// nativeChat(provider: string, messagesJson: string,
//            onEvent: (type, content) => void): number   (turn id, -1 on error)
napi_value NativeChat(napi_env env, napi_callback_info info) {
    size_t argc = 3;
    napi_value argv[3] = {nullptr, nullptr, nullptr};
    napi_get_cb_info(env, info, &argc, argv, nullptr, nullptr);
    if (argc < 3) {
        napi_throw_error(env, nullptr, "nativeChat(provider, messagesJson, onEvent) needs three arguments");
        return nullptr;
    }

    std::string provider = ToUtf8(env, argv[0]);
    std::string messages = ToUtf8(env, argv[1]);

    napi_value work_name = nullptr;
    napi_create_string_utf8(env, "mirobody_chat", NAPI_AUTO_LENGTH, &work_name);

    napi_threadsafe_function tsfn = nullptr;
    if (napi_create_threadsafe_function(env, argv[2], nullptr, work_name,
                                        0 /*unbounded*/, 1 /*one worker*/,
                                        nullptr, nullptr, nullptr, CallJs, &tsfn) != napi_ok) {
        napi_throw_error(env, nullptr, "napi_create_threadsafe_function failed");
        return nullptr;
    }

    std::shared_ptr<Turn> turn(new Turn());
    std::int32_t id;
    {
        std::lock_guard<std::mutex> lock(g_turns_mutex);
        id = g_next_turn_id++;
        g_turns[id] = turn;
    }

    std::thread(ChatWorker, id, std::move(provider), std::move(messages), tsfn, turn).detach();

    napi_value out = nullptr;
    napi_create_int32(env, id, &out);
    return out;
}

// nativeChatCancel(turnId: number): void
napi_value NativeChatCancel(napi_env env, napi_callback_info info) {
    size_t argc = 1;
    napi_value argv[1] = {nullptr};
    napi_get_cb_info(env, info, &argc, argv, nullptr, nullptr);

    std::int32_t id = 0;
    if (argc > 0) napi_get_value_int32(env, argv[0], &id);

    std::lock_guard<std::mutex> lock(g_turns_mutex);
    std::map<std::int32_t, std::shared_ptr<Turn>>::iterator it = g_turns.find(id);
    if (it != g_turns.end()) {
        it->second->cancelled.store(true, std::memory_order_relaxed);
        OH_LOG_INFO(LOG_APP, "chat turn %{public}d cancel requested", id);
    }
    return nullptr;
}

// nativeVersion(): string -- build stamp, proves the .so is really loaded.
napi_value NativeVersion(napi_env env, napi_callback_info /*info*/) {
    napi_value out = nullptr;
    napi_create_string_utf8(env, "mirobody-napi / built " __DATE__ " " __TIME__,
                            NAPI_AUTO_LENGTH, &out);
    return out;
}

}   // namespace

//------------------------------------------------------------------------------
// Module registration
//------------------------------------------------------------------------------

EXTERN_C_START
napi_value Init(napi_env env, napi_value exports) {
    napi_property_descriptor desc[] = {
        {"nativeVersion",         nullptr, NativeVersion,         nullptr, nullptr, nullptr, napi_default, nullptr},
        {"nativeSetConfig",       nullptr, NativeSetConfig,       nullptr, nullptr, nullptr, napi_default, nullptr},
        {"nativeReloadProviders", nullptr, NativeReloadProviders, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"nativeGetProviders",    nullptr, NativeGetProviders,    nullptr, nullptr, nullptr, napi_default, nullptr},
        {"nativeChat",            nullptr, NativeChat,            nullptr, nullptr, nullptr, napi_default, nullptr},
        {"nativeChatCancel",      nullptr, NativeChatCancel,      nullptr, nullptr, nullptr, napi_default, nullptr},
    };
    napi_define_properties(env, exports, sizeof(desc) / sizeof(desc[0]), desc);
    return exports;
}
EXTERN_C_END

static napi_module g_mirobody_module = {
    .nm_version = 1,
    .nm_flags = 0,
    .nm_filename = nullptr,
    .nm_register_func = Init,
    .nm_modname = "mirobody",   // must match the library name: libmirobody.so
    .nm_priv = nullptr,
    .reserved = {0},
};

extern "C" __attribute__((constructor)) void RegisterMirobodyModule(void) {
    napi_module_register(&g_mirobody_module);
}

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
#include "llm/local.hpp"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include <cerrno>
#include <cstring>
#include <dlfcn.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

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

// nativeChatAnswer(askId: string, answerJson: string): boolean
//
// Routes by the ask id from the event, NOT by turn id: the turn is parked inside a
// tool call and the C ABI resolves the waiter itself (mirobody_chat_answer), so
// nothing here has to know which turn asked. Returns false when nobody was waiting
// — a stale tap after a stop or a timeout — which the caller can ignore.
napi_value NativeChatAnswer(napi_env env, napi_callback_info info) {
    size_t argc = 2;
    napi_value argv[2] = {nullptr, nullptr};
    napi_get_cb_info(env, info, &argc, argv, nullptr, nullptr);

    const std::string ask_id = argc > 0 ? ToUtf8(env, argv[0]) : std::string();
    const std::string answer = argc > 1 ? ToUtf8(env, argv[1]) : std::string();

    const int taken = mirobody_chat_answer(ask_id.c_str(), answer.c_str());
    OH_LOG_INFO(LOG_APP, "chat answer '%{public}s' taken=%{public}d", ask_id.c_str(), taken);

    napi_value out = nullptr;
    napi_get_boolean(env, taken != 0, &out);
    return out;
}

//------------------------------------------------------------------------------
// On-device lane (src/llm/local.hpp)
//------------------------------------------------------------------------------
//
// Same threading shape as the cloud lane above, and deliberately the same tsfn /
// Turn machinery: ainvoke() blocks and calls its handler on the worker thread, so
// one detached thread per turn and one threadsafe function per turn.
//
// The client is process-wide and outlives a turn on purpose — it owns the loaded
// model, and reloading multiple GB per turn would be unusable. It is rebuilt only
// when the model path changes.

std::mutex g_local_mutex;
std::shared_ptr<mirobody::llm::LocalClient> g_local;
std::string g_local_path;
// -1 = untouched, so LocalOptions' measured default stands. A plain 0 here would
// mean "hardware_concurrency()", which is the slowest setting on this class of SoC.
std::int32_t g_local_threads = -1;

// Returns the client for `model_path`, constructing (or replacing) it if needed.
std::shared_ptr<mirobody::llm::LocalClient> LocalClientFor(const std::string& model_path) {
    std::lock_guard<std::mutex> lock(g_local_mutex);
    if (!g_local || g_local_path != model_path) {
        mirobody::llm::LocalOptions opt;
        opt.model_path = model_path;
        if (g_local_threads >= 0) {
            opt.n_threads = g_local_threads;
        }
        g_local.reset(new mirobody::llm::LocalClient(opt));
        g_local_path = model_path;
        OH_LOG_INFO(LOG_APP, "local engine bound to %{public}s", model_path.c_str());
    }
    return g_local;
}

// Minimal JSON array parse for [{role, content}, ...]. The cloud lane hands its
// JSON straight to the core, which has rapidjson; this lane needs the messages as
// C++ objects, and pulling rapidjson into the bridge for two fields is not worth
// it. Unknown keys are ignored; malformed input yields no messages, which ainvoke
// reports as an error.
std::vector<mirobody::llm::ChatMessage> ParseMessages(const std::string& json) {
    std::vector<mirobody::llm::ChatMessage> out;
    size_t i = 0;
    while (true) {
        // Each object contributes at most one role and one content string.
        const size_t obj = json.find('{', i);
        if (obj == std::string::npos) break;
        const size_t end = json.find('}', obj);
        if (end == std::string::npos) break;
        const std::string chunk = json.substr(obj, end - obj);

        mirobody::llm::ChatMessage m;
        for (int f = 0; f < 2; f++) {
            const char* key = f == 0 ? "\"role\"" : "\"content\"";
            const size_t k = chunk.find(key);
            if (k == std::string::npos) continue;
            const size_t open = chunk.find('"', chunk.find(':', k) + 1);
            if (open == std::string::npos) continue;
            std::string val;
            for (size_t p = open + 1; p < chunk.size(); p++) {
                if (chunk[p] == '\\' && p + 1 < chunk.size()) {
                    const char esc = chunk[++p];
                    val += (esc == 'n' ? '\n' : esc == 't' ? '\t' : esc);
                } else if (chunk[p] == '"') {
                    break;
                } else {
                    val += chunk[p];
                }
            }
            if (f == 0) m.role = val; else m.content = val;
        }
        if (!m.role.empty()) out.push_back(m);
        i = end + 1;
    }
    return out;
}

// The worker: run the blocking turn, emit stats + the terminal event, release the tsfn.
void LocalWorker(std::int32_t turn_id, std::string model_path, std::string messages_json,
                 napi_threadsafe_function tsfn, std::shared_ptr<Turn> turn) {
    std::shared_ptr<mirobody::llm::LocalClient> client = LocalClientFor(model_path);

    const std::vector<mirobody::llm::ChatMessage> messages = ParseMessages(messages_json);

    bool errored = false;
    const bool ok = client->ainvoke(messages, /*system_prompt=*/std::string(),
        [&](const mirobody::llm::Event& e) -> bool {
            if (turn->cancelled.load(std::memory_order_relaxed)) return false;
            // Map every type the local engine produces. Collapsing "not an error" to
            // "reply" silently undid the <think> splitting done in local.cpp — the
            // reasoning reached the UI as answer text.
            const char* type;
            switch (e.type) {
                case mirobody::llm::EventType::Error:    type = "error";    errored = true; break;
                case mirobody::llm::EventType::Thinking: type = "thinking"; break;
                default:                                 type = "reply";    break;
            }
            napi_call_threadsafe_function(tsfn, new StreamEvent{type, e.content},
                                          napi_tsfn_blocking);
            return true;
        });

    // Timings ride the same stream as a 'stats' event: the probe needs them, and a
    // separate accessor would race the turn it describes.
    const mirobody::llm::LocalStats st = client->stats();
    std::ostringstream js;
    js << "{\"loadMs\":" << (long)st.load_ms
       << ",\"prefillMs\":" << (long)st.prefill_ms
       << ",\"decodeMs\":" << (long)st.decode_ms
       << ",\"promptTokens\":" << st.prompt_tokens
       << ",\"decodedTokens\":" << st.decoded_tokens
       << ",\"threads\":" << st.n_threads
       << ",\"utf8Splits\":" << st.utf8_splits
       << ",\"droppedMsgs\":" << st.dropped_msgs
       << ",\"nCtx\":" << st.n_ctx
       << ",\"ctxMs\":" << (long)st.ctx_ms
       << ",\"backend\":\"" << mirobody::llm::LocalClient::backend() << "\"}";
    napi_call_threadsafe_function(tsfn, new StreamEvent{"stats", js.str()}, napi_tsfn_blocking);

    // Exactly one terminal event, same contract as the cloud lane: 'aborted' only
    // for a user cancel. An in-band error already went out as its own event and the
    // ArkTS side suppresses onDone after seeing one, so 'end' is correct there too.
    const char* terminal = turn->cancelled.load(std::memory_order_relaxed) ? "aborted" : "end";
    OH_LOG_INFO(LOG_APP, "local turn %{public}d finished: %{public}s (ok=%{public}d errored=%{public}d)",
                turn_id, terminal, (int)ok, (int)errored);
    napi_call_threadsafe_function(tsfn, new StreamEvent{terminal, ""}, napi_tsfn_blocking);

    napi_release_threadsafe_function(tsfn, napi_tsfn_release);

    std::lock_guard<std::mutex> lock(g_turns_mutex);
    g_turns.erase(turn_id);
}

// nativeProbePath(path): string -- JSON {ok, err, size, mmapOk}
//
// Answers the one question that decides where a GGUF may live: can NATIVE code
// open and mmap this path from inside the app sandbox? llama.cpp loads a model by
// path and mmaps it — an ArkTS-readable URI or fd is not enough. A picker grant
// (@ohos.fileshare) is URI-scoped, so whether it also makes the underlying path
// readable to fopen() is exactly what this reports. mmap is tested too: read
// access alone would still fail the loader.
napi_value NativeProbePath(napi_env env, napi_callback_info info) {
    size_t argc = 1;
    napi_value argv[1] = {nullptr};
    napi_get_cb_info(env, info, &argc, argv, nullptr, nullptr);
    const std::string path = argc > 0 ? ToUtf8(env, argv[0]) : std::string();

    std::ostringstream js;
    const int fd = ::open(path.c_str(), O_RDONLY);
    if (fd < 0) {
        js << "{\"ok\":false,\"err\":\"" << std::strerror(errno) << "\" (" << errno << ")}";
    } else {
        struct stat st{};
        const long long size = (::fstat(fd, &st) == 0) ? (long long)st.st_size : -1;
        // Map a single page: enough to prove mmap is permitted without paying for
        // a multi-GB mapping.
        bool mmap_ok = false;
        if (size > 0) {
            void* p = ::mmap(nullptr, 4096, PROT_READ, MAP_PRIVATE, fd, 0);
            if (p != MAP_FAILED) { mmap_ok = true; ::munmap(p, 4096); }
        }
        js << "{\"ok\":true,\"err\":\"\",\"size\":" << size
           << ",\"mmapOk\":" << (mmap_ok ? "true" : "false") << "}";
        ::close(fd);
    }
    OH_LOG_INFO(LOG_APP, "probePath %{public}s -> %{public}s", path.c_str(), js.str().c_str());

    napi_value out = nullptr;
    napi_create_string_utf8(env, js.str().c_str(), NAPI_AUTO_LENGTH, &out);
    return out;
}

// nativeNnrtDevices(): string -- JSON {available, err, devices:[{name, type, id}]}
//
// The gate on the whole NPU question, asked before any model is converted: does
// this device expose an accelerator to a THIRD-PARTY app through NNRt at all?
// Huawei could reasonably reserve the NPU for system apps, and if it does, every
// downstream step (MindIR conversion, a hand-written decode loop, padded static
// shapes) is wasted. `type` 3 is OH_AI_NNRTDEVICE_ACCELERATOR -- the only value
// that means NPU; 1/2 are the CPU and GPU we already have, and 0 is "other".
//
// dlopen rather than a link line on purpose: linking libmindspore_lite_ndk.so
// would make a device without it fail to load mirobody_napi, i.e. break the whole
// app to run a probe. Absent, this reports unavailable and nothing else changes.
napi_value NativeNnrtDevices(napi_env env, napi_callback_info /*info*/) {
    // Mirrors mindspore/context.h + types.h. Declared locally so the probe needs
    // no MindSpore headers at build time, which keeps it off the include path of
    // a core that must also compile for desktop.
    using Desc = void;
    using GetAll     = Desc* (*)(size_t*);
    using GetElement = Desc* (*)(Desc*, size_t);
    using DestroyAll = void  (*)(Desc**);
    using GetName    = const char* (*)(const Desc*);
    using GetType    = int   (*)(const Desc*);
    using GetId      = size_t (*)(const Desc*);

    std::ostringstream js;
    void* lib = ::dlopen("libmindspore_lite_ndk.so", RTLD_NOW | RTLD_LOCAL);
    if (!lib) {
        js << "{\"available\":false,\"err\":\"dlopen: " << (::dlerror() ? ::dlerror() : "?")
           << "\",\"devices\":[]}";
    } else {
        auto get_all  = (GetAll)    ::dlsym(lib, "OH_AI_GetAllNNRTDeviceDescs");
        auto get_elem = (GetElement)::dlsym(lib, "OH_AI_GetElementOfNNRTDeviceDescs");
        auto destroy  = (DestroyAll)::dlsym(lib, "OH_AI_DestroyAllNNRTDeviceDescs");
        auto get_name = (GetName)   ::dlsym(lib, "OH_AI_GetNameFromNNRTDeviceDesc");
        auto get_type = (GetType)   ::dlsym(lib, "OH_AI_GetTypeFromNNRTDeviceDesc");
        auto get_id   = (GetId)     ::dlsym(lib, "OH_AI_GetDeviceIdFromNNRTDeviceDesc");

        if (!get_all || !get_elem || !get_name || !get_type) {
            js << "{\"available\":false,\"err\":\"dlsym failed\",\"devices\":[]}";
        } else {
            size_t num = 0;
            Desc* descs = get_all(&num);
            js << "{\"available\":true,\"err\":\"\",\"devices\":[";
            for (size_t i = 0; descs && i < num; ++i) {
                Desc* d = get_elem(descs, i);
                if (!d) continue;
                const char* name = get_name(d);
                if (i) js << ",";
                js << "{\"name\":\"" << (name ? name : "") << "\",\"type\":" << get_type(d)
                   << ",\"id\":" << (get_id ? (unsigned long long)get_id(d) : 0ULL) << "}";
            }
            js << "]}";
            if (descs && destroy) destroy(&descs);
        }
        ::dlclose(lib);
    }
    OH_LOG_INFO(LOG_APP, "nnrtDevices %{public}s", js.str().c_str());

    napi_value out = nullptr;
    napi_create_string_utf8(env, js.str().c_str(), NAPI_AUTO_LENGTH, &out);
    return out;
}

// Defined in membw.cpp, which the build forces to -O2. At the app's Debug -O0 the
// read loop measures loop overhead rather than DRAM.
extern "C" void   mirobody_membw_touch(std::uint64_t* buf, std::size_t n);
extern "C" double mirobody_membw_read_gbs(const std::uint64_t* buf, std::size_t n, int threads);

// nativeMemBandwidth(): string -- JSON {mb, threads, oneThreadGbs, allThreadGbs}
//
// Sequential-read bandwidth this SoC actually delivers, which sets a HARD ceiling
// on decode that no accelerator can lift. Autoregressive decode reads essentially
// every weight per token, so tok/s <= bandwidth / model_bytes -- and an NPU shares
// the same LPDDR controller as the CPU. This is the measurement that explains why
// the Vulkan GPU lost, made before spending anything on an NPU port: if measured
// bandwidth is near what decode already consumes (2B Q4_K_M at 11.2 tok/s is
// ~14.3 GB/s), then decode is at the wall, prefill is not our bottleneck, and the
// whole NPU path has no upside regardless of op coverage.
//
// Buffer is far larger than any cache so this measures DRAM, not SRAM, and the
// thread count matches what a turn actually uses -- a single thread cannot
// saturate a modern memory controller, so a one-thread number alone would
// understate the ceiling.
napi_value NativeMemBandwidth(napi_env env, napi_callback_info /*info*/) {
    constexpr size_t kBytes = 256ull << 20;   // 256 MB
    const int threads = g_local_threads > 0 ? g_local_threads : 6;

    auto* buf = static_cast<std::uint64_t*>(
        ::mmap(nullptr, kBytes, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0));
    if (buf == MAP_FAILED) {
        napi_value out = nullptr;
        napi_create_string_utf8(env, "{\"err\":\"mmap failed\"}", NAPI_AUTO_LENGTH, &out);
        return out;
    }
    const size_t n = kBytes / sizeof(std::uint64_t);
    mirobody_membw_touch(buf, n);

    const double one = mirobody_membw_read_gbs(buf, n, 1);
    const double all = mirobody_membw_read_gbs(buf, n, threads);
    ::munmap(buf, kBytes);

    std::ostringstream js;
    js.setf(std::ios::fixed); js.precision(1);
    js << "{\"mb\":" << (kBytes >> 20) << ",\"threads\":" << threads
       << ",\"oneThreadGbs\":" << one << ",\"allThreadGbs\":" << all << "}";
    OH_LOG_INFO(LOG_APP, "memBandwidth %{public}s", js.str().c_str());

    napi_value out = nullptr;
    napi_create_string_utf8(env, js.str().c_str(), NAPI_AUTO_LENGTH, &out);
    return out;
}

// nativeLocalSetThreads(n): void -- 0/negative means hardware_concurrency().
//
// A knob rather than a constant because the right value is a property of the SoC:
// on a big.LITTLE phone, including the little cores in a matmul can cost more than
// it adds. Applies from the next turn; the model is not reloaded.
napi_value NativeLocalSetThreads(napi_env env, napi_callback_info info) {
    size_t argc = 1;
    napi_value argv[1] = {nullptr};
    napi_get_cb_info(env, info, &argc, argv, nullptr, nullptr);
    std::int32_t n = 0;
    if (argc > 0) napi_get_value_int32(env, argv[0], &n);

    std::lock_guard<std::mutex> lock(g_local_mutex);
    if (g_local) {
        g_local->setThreads(n);
    }
    g_local_threads = n;   // remembered so a client built later starts with it
    OH_LOG_INFO(LOG_APP, "local threads set to %{public}d", n);
    return nullptr;
}

// nativeLocalStatus(): string -- JSON {available, backend, loaded, modelPath}
napi_value NativeLocalStatus(napi_env env, napi_callback_info /*info*/) {
    std::string path;
    bool loaded = false;
    {
        std::lock_guard<std::mutex> lock(g_local_mutex);
        path = g_local_path;
        loaded = g_local && g_local->loaded();
    }
    // Two groups on purpose: `has` is the silicon (AT_HWCAP), `built` is what this
    // binary emits. has && !built is unrealized performance — the exact gap a
    // flagless cross build creates. /proc/cpuinfo is unreadable to an app here.
    const mirobody::llm::LocalCpuFeatures f = mirobody::llm::LocalClient::cpuFeatures();
    std::ostringstream js;
    js << "{\"available\":" << (mirobody::llm::LocalClient::available() ? "true" : "false")
       << ",\"backend\":\"" << mirobody::llm::LocalClient::backend() << "\""
       << ",\"devices\":\"" << mirobody::llm::LocalClient::devices() << "\""
       << ",\"loaded\":" << (loaded ? "true" : "false")
       << ",\"modelPath\":\"" << path << "\""
       << ",\"has\":{"
       << "\"fp16\":"     << (f.has_fp16    ? "true" : "false")
       << ",\"dotprod\":" << (f.has_dotprod ? "true" : "false")
       << ",\"i8mm\":"    << (f.has_i8mm    ? "true" : "false")
       << ",\"bf16\":"    << (f.has_bf16    ? "true" : "false")
       << ",\"sve\":"     << (f.has_sve     ? "true" : "false")
       << ",\"sve2\":"    << (f.has_sve2    ? "true" : "false")
       << ",\"sme\":"     << (f.has_sme     ? "true" : "false")
       << "},\"built\":{"
       << "\"neon\":"     << (f.built_neon    ? "true" : "false")
       << ",\"fma\":"     << (f.built_fma     ? "true" : "false")
       << ",\"fp16\":"    << (f.built_fp16    ? "true" : "false")
       << ",\"dotprod\":" << (f.built_dotprod ? "true" : "false")
       << ",\"i8mm\":"    << (f.built_i8mm    ? "true" : "false")
       << ",\"sve\":"     << (f.built_sve     ? "true" : "false")
       << ",\"sme\":"     << (f.built_sme     ? "true" : "false")
       << "}}";
    OH_LOG_INFO(LOG_APP, "localStatus %{public}s", js.str().c_str());

    napi_value out = nullptr;
    napi_create_string_utf8(env, js.str().c_str(), NAPI_AUTO_LENGTH, &out);
    return out;
}

// nativeLocalChat(modelPath, messagesJson, onEvent): number  (turn id)
napi_value NativeLocalChat(napi_env env, napi_callback_info info) {
    size_t argc = 3;
    napi_value argv[3] = {nullptr, nullptr, nullptr};
    napi_get_cb_info(env, info, &argc, argv, nullptr, nullptr);
    if (argc < 3) {
        napi_throw_error(env, nullptr,
                         "nativeLocalChat(modelPath, messagesJson, onEvent) needs three arguments");
        return nullptr;
    }

    std::string model_path = ToUtf8(env, argv[0]);
    std::string messages   = ToUtf8(env, argv[1]);

    napi_value work_name = nullptr;
    napi_create_string_utf8(env, "mirobody_local_chat", NAPI_AUTO_LENGTH, &work_name);

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

    std::thread(LocalWorker, id, std::move(model_path), std::move(messages), tsfn, turn).detach();

    napi_value out = nullptr;
    napi_create_int32(env, id, &out);
    return out;
}

//------------------------------------------------------------------------------
// Health data (on-device ingest)
//------------------------------------------------------------------------------
//
// Both calls hit SQLite, so both return a PROMISE and do the work on a libuv
// worker (napi_create_async_work) rather than the ArkTS thread: a sync writes one
// row per reading and a week of samples is hundreds of them, which is exactly the
// kind of stall that shows up as a frozen list mid-scroll.
//
// Why async_work here and a tsfn for chat: a chat turn streams many events back
// and needs a channel; these produce ONE result, and a promise is what ArkTS
// wants for that. `user_id` 0 is passed through -- the core resolves it to the
// device owner (row 1), the same subject a chat turn runs as, which is what makes
// the family_health tool able to read what a sync wrote.

struct HealthWork {
    napi_async_work work     = nullptr;
    napi_deferred   deferred = nullptr;
    bool            is_store = false;
    std::string     resources;   // store: the Observations JSON
    std::int32_t    count    = 0;// recent: how many rows
    std::string     result;      // the core's JSON answer
    bool            ok       = false;
};

// Worker thread: NO napi calls in here, only the C ABI.
void HealthExecute(napi_env /*env*/, void* data) {
    HealthWork* w = static_cast<HealthWork*>(data);
    const char* out = w->is_store
        ? mirobody_health_store(/*user_id=*/0, w->resources.c_str())
        : mirobody_health_recent(/*user_id=*/0, w->count);
    // The buffer is owned by the core and valid only until this thread's next
    // health call -- copy it here, while we still hold it.
    if (out != nullptr) {
        w->result = out;
        w->ok = true;
    }
}

// Back on the ArkTS thread: settle the promise and free the work item.
void HealthComplete(napi_env env, napi_status status, void* data) {
    std::unique_ptr<HealthWork> w(static_cast<HealthWork*>(data));
    if (w->ok && status == napi_ok) {
        napi_value value = nullptr;
        napi_create_string_utf8(env, w->result.c_str(), NAPI_AUTO_LENGTH, &value);
        napi_resolve_deferred(env, w->deferred, value);
    } else {
        // NULL from the core means no database configured or unusable input; it
        // has already logged the specifics. Reject so ArkTS sees a failure rather
        // than an empty result it would read as "no data".
        napi_value msg = nullptr, err = nullptr;
        napi_create_string_utf8(env,
            w->is_store ? "nativeHealthStore failed (no database, or resources JSON is not an array)"
                        : "nativeHealthRecent failed (no database configured)",
            NAPI_AUTO_LENGTH, &msg);
        napi_create_error(env, nullptr, msg, &err);
        napi_reject_deferred(env, w->deferred, err);
    }
    napi_delete_async_work(env, w->work);
}

// Queue one HealthWork and hand back its promise.
napi_value QueueHealthWork(napi_env env, HealthWork* w, const char* name) {
    napi_value promise = nullptr;
    if (napi_create_promise(env, &w->deferred, &promise) != napi_ok) {
        delete w;
        napi_throw_error(env, nullptr, "napi_create_promise failed");
        return nullptr;
    }
    napi_value work_name = nullptr;
    napi_create_string_utf8(env, name, NAPI_AUTO_LENGTH, &work_name);
    if (napi_create_async_work(env, nullptr, work_name, HealthExecute, HealthComplete,
                               w, &w->work) != napi_ok) {
        delete w;
        napi_throw_error(env, nullptr, "napi_create_async_work failed");
        return nullptr;
    }
    napi_queue_async_work(env, w->work);
    return promise;
}

// nativeHealthStore(resourcesJson: string): Promise<string>
napi_value NativeHealthStore(napi_env env, napi_callback_info info) {
    size_t argc = 1;
    napi_value argv[1] = {nullptr};
    napi_get_cb_info(env, info, &argc, argv, nullptr, nullptr);
    if (argc < 1) {
        napi_throw_error(env, nullptr, "nativeHealthStore(resourcesJson) needs one argument");
        return nullptr;
    }
    HealthWork* w = new HealthWork();
    w->is_store = true;
    w->resources = ToUtf8(env, argv[0]);
    return QueueHealthWork(env, w, "mirobody_health_store");
}

// nativeHealthRecent(count: number): Promise<string>
napi_value NativeHealthRecent(napi_env env, napi_callback_info info) {
    size_t argc = 1;
    napi_value argv[1] = {nullptr};
    napi_get_cb_info(env, info, &argc, argv, nullptr, nullptr);

    HealthWork* w = new HealthWork();
    w->is_store = false;
    if (argc > 0) {
        napi_get_value_int32(env, argv[0], &w->count);
    }
    return QueueHealthWork(env, w, "mirobody_health_recent");
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
        {"nativeChatAnswer",      nullptr, NativeChatAnswer,      nullptr, nullptr, nullptr, napi_default, nullptr},
        {"nativeProbePath",       nullptr, NativeProbePath,       nullptr, nullptr, nullptr, napi_default, nullptr},
        {"nativeNnrtDevices",     nullptr, NativeNnrtDevices,     nullptr, nullptr, nullptr, napi_default, nullptr},
        {"nativeMemBandwidth",    nullptr, NativeMemBandwidth,    nullptr, nullptr, nullptr, napi_default, nullptr},
        {"nativeLocalStatus",     nullptr, NativeLocalStatus,     nullptr, nullptr, nullptr, napi_default, nullptr},
        {"nativeLocalChat",       nullptr, NativeLocalChat,       nullptr, nullptr, nullptr, napi_default, nullptr},
        {"nativeLocalSetThreads", nullptr, NativeLocalSetThreads, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"nativeHealthStore",     nullptr, NativeHealthStore,     nullptr, nullptr, nullptr, napi_default, nullptr},
        {"nativeHealthRecent",    nullptr, NativeHealthRecent,    nullptr, nullptr, nullptr, napi_default, nullptr},
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

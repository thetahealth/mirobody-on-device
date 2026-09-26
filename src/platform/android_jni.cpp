#if defined(__ANDROID__)

#include "client/http_client.hpp"
#include "llm/local.hpp"
#include "config/config.hpp"
#include "platform/log.hpp"
#include "server/server.hpp"

#include <jni.h>
#include <libwebsockets.h>

#include <cstdio>
#include <cstdlib>
#include <memory>
#include <mutex>
#include <new>
#include <string>
#include <vector>

namespace {

std::once_flag g_curl_init_flag;

void ensure_curl_init() {
    std::call_once(g_curl_init_flag, [] {
        mirobody::client::HttpClient::global_init();
    });
}

void lws_log_to_android(int level, const char* line) {
    (void)level;
    mirobody::platform::log_info("%s", line);
}

std::string jstring_to_std(JNIEnv* env, jstring s) {
    if (!s) return {};
    const char* c = env->GetStringUTFChars(s, nullptr);
    std::string out = c ? std::string(c) : std::string{};
    if (c) env->ReleaseStringUTFChars(s, c);
    return out;
}

}

//------------------------------------------------------------------------------
// JNI exports
//------------------------------------------------------------------------------

extern "C" {

JNIEXPORT jlong JNICALL
Java_ai_thetahealth_mirobody_NativeBridge_nativeStart(
        JNIEnv* env, jobject /*thiz*/,
        jstring jConfigPath,
        jstring jDataDir,
        jstring jOpenaiKey,
        jstring jGeminiKey,
        jint listenPort) {

    ensure_curl_init();

    int log_mask = LLL_ERR | LLL_WARN;
    lws_set_log_level(log_mask, lws_log_to_android);

    std::string cfg_path = jstring_to_std(env, jConfigPath);
    std::string data_dir = jstring_to_std(env, jDataDir);
    std::string openai_key = jstring_to_std(env, jOpenaiKey);
    std::string gemini_key = jstring_to_std(env, jGeminiKey);

    mirobody::Config cfg;
    try {
        if (!cfg_path.empty()) {
            cfg = mirobody::load_config(cfg_path);
        } else {
            cfg = mirobody::load_config();
        }
    } catch (const std::exception& e) {
        mirobody::platform::log_error("config error: %s", e.what());
        return 0;
    }

    if (!openai_key.empty()) cfg.openai.api_key = std::move(openai_key);
    if (!gemini_key.empty()) cfg.gemini.api_key = std::move(gemini_key);
    if (listenPort > 0) cfg.listen_port = static_cast<std::uint16_t>(listenPort);
    // The host app reaches the core over loopback only. The compiled default is
    // 0.0.0.0, which here would put the phone's health record on every network
    // the phone joins, so it is overridden just as ios_bridge.mm does.
    if (cfg.listen_addr.empty() || cfg.listen_addr == "0.0.0.0") {
        cfg.listen_addr = "127.0.0.1";
    }

    auto server = std::unique_ptr<mirobody::Server>(new mirobody::Server(std::move(cfg)));
    if (!server->start_in_background()) {
        mirobody::platform::log_error("server failed to start");
        return 0;
    }
    return reinterpret_cast<jlong>(server.release());
}

//------------------------------------------------------------------------------

JNIEXPORT void JNICALL
Java_ai_thetahealth_mirobody_NativeBridge_nativeStop(
        JNIEnv* /*env*/, jobject /*thiz*/, jlong handle) {
    if (!handle) return;
    auto* server = reinterpret_cast<mirobody::Server*>(handle);
    server->stop_and_join();
    delete server;
}

//------------------------------------------------------------------------------

JNIEXPORT jboolean JNICALL
Java_ai_thetahealth_mirobody_NativeBridge_nativeIsRunning(
        JNIEnv* /*env*/, jobject /*thiz*/, jlong handle) {
    if (!handle) return JNI_FALSE;
    auto* server = reinterpret_cast<mirobody::Server*>(handle);
    return server->is_running() ? JNI_TRUE : JNI_FALSE;
}

//------------------------------------------------------------------------------

JNIEXPORT jint JNICALL
Java_ai_thetahealth_mirobody_NativeBridge_nativeListenPort(
        JNIEnv* /*env*/, jobject /*thiz*/, jlong handle) {
    if (!handle) return -1;
    auto* server = reinterpret_cast<mirobody::Server*>(handle);
    return static_cast<jint>(server->config().listen_port);
}

//------------------------------------------------------------------------------
// On-device LLM (llama.cpp + GGUF)
//
// The second on-device runtime, beside LiteRT-LM. Everything below is a thin shell
// over llm::LocalClient, which HarmonyOS already drives through its NAPI bridge --
// same client, same options, so the two mobile clients cannot drift on threading or
// context sizing. Guarded by MIROBODY_ONDEVICE_LLM at the CMake level: without it
// local.cpp compiles a stub whose available() is false, and these entry points still
// link and still answer honestly.
//------------------------------------------------------------------------------

/// Is the engine compiled in, and does what we COMPILED FOR match what the chip has?
///
/**
 * Tell llama.cpp where its dlopen-able backends live, before anything else touches it.
 *
 * ApplicationInfo.nativeLibraryDir, which only Java can name -- ggml's own search looks
 * beside the executable, and on Android that is /system/bin/app_process.
 */
JNIEXPORT void JNICALL
Java_ai_thetahealth_mirobody_NativeBridge_nativeLocalBackendPath(
        JNIEnv* env, jobject /*thiz*/, jstring dir) {
    mirobody::llm::LocalClient::backendPath(jstring_to_std(env, dir));
}

/// `has_*` is the silicon (AT_HWCAP). What it is paired against depends on the build:
///
///   - statically linked: `built_*`, ggml's compile-time view. A cross build with the
///     wrong -march silently drops the fast kernels and nothing else reports it, so
///     `has` true with `built` false is performance left on the table.
///   - GGML_CPU_ALL_VARIANTS (`dispatch`): there is no build-time answer -- ggml picked
///     a module at startup, and `variant` names it. An EMPTY variant in a dispatch build
///     is the failure to look for: the modules were not found, so ggml fell back to
///     whatever it could, and the only symptom is slowness.
JNIEXPORT jstring JNICALL
Java_ai_thetahealth_mirobody_NativeBridge_nativeLocalStatus(JNIEnv* env, jobject /*thiz*/) {
    const mirobody::llm::LocalCpuFeatures f = mirobody::llm::LocalClient::cpuFeatures();
    auto b = [](bool v) { return v ? "true" : "false"; };
#ifdef MIROBODY_ONDEVICE_LLM_DL
    const char* dispatch = "true";
#else
    const char* dispatch = "false";
#endif
    char buf[1024];
    std::snprintf(buf, sizeof(buf),
                  "{\"available\":%s,\"backend\":\"%s\",\"devices\":\"%s\","
                  "\"dispatch\":%s,\"variant\":\"%s\","
                  "\"has\":{\"fp16\":%s,\"dotprod\":%s,\"i8mm\":%s,\"bf16\":%s,"
                  "\"sve\":%s,\"sve2\":%s,\"sme\":%s},"
                  "\"built\":{\"neon\":%s,\"fma\":%s,\"fp16\":%s,\"dotprod\":%s,"
                  "\"i8mm\":%s,\"sve\":%s,\"sme\":%s}}",
                  b(mirobody::llm::LocalClient::available()),
                  mirobody::llm::LocalClient::backend(),
                  mirobody::llm::LocalClient::devices().c_str(),
                  dispatch, f.variant.c_str(),
                  b(f.has_fp16), b(f.has_dotprod), b(f.has_i8mm), b(f.has_bf16),
                  b(f.has_sve), b(f.has_sve2), b(f.has_sme),
                  b(f.built_neon), b(f.built_fma), b(f.built_fp16), b(f.built_dotprod),
                  b(f.built_i8mm), b(f.built_sve), b(f.built_sme));
    return env->NewStringUTF(buf);
}

/**
 * Run one timed turn and report llm::LocalStats as JSON.
 *
 * Shaped to be comparable with LiteRT-LM's own benchmark(): same idea, same
 * numbers -- load, prefill and decode measured separately, in tokens per second.
 * Reply text is discarded; only the timings are wanted.
 *
 * BLOCKS for the whole turn (seconds to minutes, and a cold load reads a few GB),
 * so the caller must be off the main thread. A fresh LocalClient per call is
 * deliberate: reusing one would hide the load cost, which is half of what is
 * being measured.
 */
JNIEXPORT jstring JNICALL
Java_ai_thetahealth_mirobody_NativeBridge_nativeLocalBenchmark(
        JNIEnv* env, jobject /*thiz*/,
        jstring model_path, jstring prompt, jint decode_tokens, jint threads) {
    const std::string path = jstring_to_std(env, model_path);
    const std::string text = jstring_to_std(env, prompt);

    mirobody::llm::LocalOptions opt;
    opt.model_path = path;
    // Cap the reply so the run is bounded and comparable, not so the model is
    // limited: the benchmark wants a fixed amount of decode, nothing more.
    if (decode_tokens > 0) opt.n_predict = decode_tokens;
    if (threads > 0)       opt.n_threads = threads;

    mirobody::llm::LocalClient client(opt);
    std::vector<mirobody::llm::ChatMessage> messages;
    messages.push_back(mirobody::llm::ChatMessage{"user", text});

    std::string error;
    const bool ok = client.ainvoke(
        messages, std::string(),
        [&error](const mirobody::llm::Event& e) {
            if (e.type == mirobody::llm::EventType::Error) error = e.content;
            return true;   // discard the reply; only the clock matters here
        });

    const mirobody::llm::LocalStats st = client.stats();
    // tok/s computed here rather than in Kotlin so the division lives next to the
    // fields it divides, and a zero duration cannot become an Infinity on the way out.
    const double prefill_tps = st.prefill_ms > 0 ? st.prompt_tokens  * 1000.0 / st.prefill_ms : 0.0;
    const double decode_tps  = st.decode_ms  > 0 ? st.decoded_tokens * 1000.0 / st.decode_ms  : 0.0;

    char buf[768];
    std::snprintf(buf, sizeof(buf),
                  "{\"ok\":%s,\"error\":\"%s\",\"loadMs\":%.1f,\"prefillMs\":%.1f,"
                  "\"decodeMs\":%.1f,\"promptTokens\":%d,\"decodedTokens\":%d,"
                  "\"prefillTps\":%.2f,\"decodeTps\":%.2f,\"threads\":%d,\"nCtx\":%d}",
                  ok ? "true" : "false", error.c_str(),
                  st.load_ms, st.prefill_ms, st.decode_ms,
                  st.prompt_tokens, st.decoded_tokens,
                  prefill_tps, decode_tps, st.n_threads, st.n_ctx);
    return env->NewStringUTF(buf);
}

//------------------------------------------------------------------------------
// Streaming chat over llama.cpp
//
// Handle-based rather than one call per turn, because the model must NOT be re-read
// between turns: that is seconds and a few GB. The handle is a LocalClient, which owns
// the weights for its life and builds a fresh context per turn.
//
// Events cross back through a Kotlin object with `boolean onEvent(int, String)`, looked
// up by signature rather than by a fixed class name so the callback can be any
// implementation. Returning false from it cancels the turn at the next token -- that is
// how a cancelled coroutine stops a decode loop that would otherwise run for minutes.
//
// No JavaVM/AttachCurrentThread anywhere: ainvoke() is documented as caller-threaded and
// calls its handler inline, so every callback lands on the very thread that entered JNI
// and its JNIEnv is already valid. Kotlin's side of the contract is to call this from a
// background dispatcher, since it blocks for the whole turn.
//------------------------------------------------------------------------------

/// Event codes on the wire. Kotlin mirrors these; see NativeBridge.LocalEventSink.
enum : jint { EV_REPLY = 0, EV_THINKING = 1, EV_ERROR = 2 };

JNIEXPORT jlong JNICALL
Java_ai_thetahealth_mirobody_NativeBridge_nativeLocalOpen(
        JNIEnv* env, jobject /*thiz*/, jstring model_path, jint threads, jint thinking) {
    mirobody::llm::LocalOptions opt;
    opt.model_path = jstring_to_std(env, model_path);
    if (threads > 0) { opt.n_threads = threads; }
    opt.thinking = thinking;
    return reinterpret_cast<jlong>(new (std::nothrow) mirobody::llm::LocalClient(std::move(opt)));
}

JNIEXPORT void JNICALL
Java_ai_thetahealth_mirobody_NativeBridge_nativeLocalClose(
        JNIEnv* /*env*/, jobject /*thiz*/, jlong handle) {
    delete reinterpret_cast<mirobody::llm::LocalClient*>(handle);
}

/// Stop the in-flight turn. Safe from another thread -- that is the point of it.
JNIEXPORT void JNICALL
Java_ai_thetahealth_mirobody_NativeBridge_nativeLocalCancel(
        JNIEnv* /*env*/, jobject /*thiz*/, jlong handle) {
    if (auto* c = reinterpret_cast<mirobody::llm::LocalClient*>(handle)) { c->cancel(); }
}

/// Read the weights now. Returns "" on success, else why not. BLOCKS for seconds.
JNIEXPORT jstring JNICALL
Java_ai_thetahealth_mirobody_NativeBridge_nativeLocalLoad(
        JNIEnv* env, jobject /*thiz*/, jlong handle) {
    auto* c = reinterpret_cast<mirobody::llm::LocalClient*>(handle);
    if (!c) { return env->NewStringUTF("no engine"); }
    std::string err;
    if (c->load(err)) { return env->NewStringUTF(""); }
    return env->NewStringUTF(err.empty() ? "could not load model" : err.c_str());
}

/// True once the weights are in memory, so the caller knows whether to show a spinner.
JNIEXPORT jboolean JNICALL
Java_ai_thetahealth_mirobody_NativeBridge_nativeLocalLoaded(
        JNIEnv* /*env*/, jobject /*thiz*/, jlong handle) {
    auto* c = reinterpret_cast<mirobody::llm::LocalClient*>(handle);
    return (c && c->loaded()) ? JNI_TRUE : JNI_FALSE;
}

/**
 * Run one turn, streaming into `sink`. BLOCKS until the reply ends or is cancelled.
 *
 * `roles` and `contents` are parallel arrays rather than a JSON blob: the transcript is
 * already two strings per turn on the Kotlin side, and encoding it only to parse it back
 * would add an escaping bug for nothing.
 *
 * `<think>` is split in C++ (llm::ThinkSplitter), so Thinking arrives as its own event
 * and the caller never sees a tag. Deliberately NOT symmetric with the LiteRT path, which
 * splits in Kotlin -- there the tokens only ever exist on that side.
 */
JNIEXPORT jboolean JNICALL
Java_ai_thetahealth_mirobody_NativeBridge_nativeLocalGenerate(
        JNIEnv* env, jobject /*thiz*/, jlong handle,
        jobjectArray roles, jobjectArray contents, jstring system_prompt, jobject sink) {
    auto* client = reinterpret_cast<mirobody::llm::LocalClient*>(handle);
    if (!client || !sink) { return JNI_FALSE; }

    jclass sink_cls = env->GetObjectClass(sink);
    jmethodID on_event = env->GetMethodID(sink_cls, "onEvent", "(ILjava/lang/String;)Z");
    if (!on_event) { return JNI_FALSE; }

    std::vector<mirobody::llm::ChatMessage> messages;
    const jsize n = env->GetArrayLength(roles);
    messages.reserve(static_cast<size_t>(n));
    for (jsize i = 0; i < n; i++) {
        auto r = static_cast<jstring>(env->GetObjectArrayElement(roles, i));
        auto c = static_cast<jstring>(env->GetObjectArrayElement(contents, i));
        messages.push_back(mirobody::llm::ChatMessage{jstring_to_std(env, r),
                                                      jstring_to_std(env, c)});
        env->DeleteLocalRef(r);
        env->DeleteLocalRef(c);
    }

    const bool ok = client->ainvoke(
        messages, jstring_to_std(env, system_prompt),
        [env, sink, on_event](const mirobody::llm::Event& e) {
            jint code;
            switch (e.type) {
                case mirobody::llm::EventType::Reply:    code = EV_REPLY;    break;
                case mirobody::llm::EventType::Thinking: code = EV_THINKING; break;
                case mirobody::llm::EventType::Error:    code = EV_ERROR;    break;
                // Tool and cost events cannot arise on a local turn; ignore rather than
                // invent a code the other side would only have to learn to discard.
                default: return true;
            }
            jstring text = env->NewStringUTF(e.content.c_str());
            const jboolean keep = env->CallBooleanMethod(sink, on_event, code, text);
            // Per token, for a whole reply: without this the local-reference table fills
            // and the VM aborts partway through a long answer.
            env->DeleteLocalRef(text);
            if (env->ExceptionCheck()) {
                env->ExceptionClear();
                return false;
            }
            return keep == JNI_TRUE;
        });
    return ok ? JNI_TRUE : JNI_FALSE;
}

}

#endif

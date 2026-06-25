#if defined(__ANDROID__)

#include "client/http_client.hpp"
#include "config/config.hpp"
#include "platform/log.hpp"
#include "server/server.hpp"

#include <jni.h>
#include <libwebsockets.h>

#include <cstdlib>
#include <memory>
#include <mutex>
#include <new>
#include <string>

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
    if (cfg.listen_addr.empty()) cfg.listen_addr = "127.0.0.1";

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

}

#endif

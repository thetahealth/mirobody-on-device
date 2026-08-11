// The C door onto llm::LocalClient — the on-device LLM half of mirobody.h.
//
// In the CORE rather than beside c_api.cpp, and that placement is the point. The
// three platform bridges are mutually exclusive: c_api.cpp is compiled only into
// the desktop shared library, ios_bridge.mm only into the iOS static lib,
// android_jni.cpp only into the Android .so. Nothing in this file is
// platform-specific, so putting it in any one of them would mean either
// duplicating it or leaving iOS — the host that actually needs it — without.
//
// Every one of those targets links mirobody_core, so one definition reaches all
// three with no #if to keep in sync.
//
// The shape deliberately mirrors the JNI entry points in android_jni.cpp: open,
// load, generate, cancel, close. They are two spellings of one contract, and a
// divergence between them would be a bug rather than a design.

#include "mirobody.h"

#include "llm/local.hpp"

#include <string>
#include <vector>

namespace {

// The engine, wearing an opaque C type. A struct rather than a cast straight to
// LocalClient* so a stale handle from an older build fails at the type rather
// than by dereferencing whatever it used to point at.
struct llm_handle {
    mirobody::llm::LocalClient client;
    explicit llm_handle(mirobody::llm::LocalOptions opt) : client(std::move(opt)) {}
};

llm_handle* as_handle(mirobody_llm_t* h) { return reinterpret_cast<llm_handle*>(h); }

}  // namespace

extern "C" int mirobody_llm_available(void) {
    return mirobody::llm::LocalClient::available() ? 1 : 0;
}

extern "C" void mirobody_llm_backend_path(const char* dir) {
    mirobody::llm::LocalClient::backendPath(dir ? std::string(dir) : std::string());
}

extern "C" mirobody_llm_t* mirobody_llm_open(const char* model_path, int threads, int thinking) {
    if (!model_path || !*model_path) { return nullptr; }
    if (!mirobody::llm::LocalClient::available()) { return nullptr; }
    mirobody::llm::LocalOptions opt;
    opt.model_path = model_path;
    // <= 0 keeps the library's own default, which is NOT hardware_concurrency:
    // on a big.LITTLE phone handing the little cores a share of a matmul is a net
    // loss, because the batch waits on the slowest thread. See LocalOptions.
    if (threads > 0) { opt.n_threads = threads; }
    opt.thinking = thinking;
    try {
        return reinterpret_cast<mirobody_llm_t*>(new llm_handle(std::move(opt)));
    } catch (...) {
        // A host that gets NULL reports "could not start the engine"; a host that
        // gets a C++ exception across the ABI gets undefined behaviour.
        return nullptr;
    }
}

extern "C" void mirobody_llm_close(mirobody_llm_t* handle) {
    delete as_handle(handle);
}

extern "C" void mirobody_llm_cancel(mirobody_llm_t* handle) {
    if (handle) { as_handle(handle)->client.cancel(); }
}

extern "C" const char* mirobody_llm_load(mirobody_llm_t* handle) {
    // Owned by the library and returned to the caller: thread_local so the pointer
    // is stable until this thread calls again, with no cross-thread race. Same
    // contract as mirobody_get_providers and the health calls.
    static thread_local std::string result;
    result.clear();
    if (!handle) {
        result = "no engine";
        return result.c_str();
    }
    std::string err;
    if (!as_handle(handle)->client.load(err)) {
        result = err.empty() ? "could not load model" : err;
    }
    return result.c_str();
}

extern "C" int mirobody_llm_loaded(mirobody_llm_t* handle) {
    return (handle && as_handle(handle)->client.loaded()) ? 1 : 0;
}

extern "C" int mirobody_llm_generate(
    mirobody_llm_t* handle,
    const char* const* roles,
    const char* const* contents,
    int count,
    const char* system_prompt,
    mirobody_llm_handler on_event,
    void* user_data) {
    if (!handle || !on_event || count < 0) { return 0; }
    if (count > 0 && (!roles || !contents)) { return 0; }

    std::vector<mirobody::llm::ChatMessage> messages;
    messages.reserve(static_cast<size_t>(count));
    for (int i = 0; i < count; i++) {
        messages.push_back(mirobody::llm::ChatMessage{
            roles[i] ? roles[i] : "user",
            contents[i] ? contents[i] : "",
        });
    }

    const bool ok = as_handle(handle)->client.ainvoke(
        messages, system_prompt ? std::string(system_prompt) : std::string(),
        [on_event, user_data](const mirobody::llm::Event& e) {
            int kind;
            switch (e.type) {
                case mirobody::llm::EventType::Reply:    kind = MIROBODY_LLM_REPLY;    break;
                case mirobody::llm::EventType::Thinking: kind = MIROBODY_LLM_THINKING; break;
                case mirobody::llm::EventType::Error:    kind = MIROBODY_LLM_ERROR;    break;
                // Tool and cost events cannot arise on a local turn. Dropped rather
                // than given a code every caller would then have to learn to ignore.
                default: return true;
            }
            return on_event(kind, e.content.c_str(), user_data) != 0;
        });
    return ok ? 1 : 0;
}

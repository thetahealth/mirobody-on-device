#include "locallmengine.hpp"

#include <QString>

#ifdef MIROBODY_ONDEVICE_LLM

// ---------------------------------------------------------------------------
// Real engine — links llama.cpp (its `llama` shared library + llama.h). This is
// the same runtime the Electron desktop client uses (node-llama-cpp), so both
// desktop clients run the same Gemma GGUF. Pure-CMake build, prebuilt libs exist;
// the C API links from either an MSVC or a GCC/Clang build of the Qt client.
// Configure with -DMIROBODY_ONDEVICE_LLM=ON -DLLAMA_CPP_DIR=<dir with
// include/llama.h + lib/llama.{lib,dll}>. See qt/README.md.
// ---------------------------------------------------------------------------
#include <atomic>
#include <cstdint>
#include <cstring>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "llama.h"

namespace {
constexpr char kSystemPrompt[] =
    "You are Mirobody's private on-device health assistant. Answer concisely. "
    "You have no internet or tools; rely only on the conversation.";
constexpr int kContextTokens   = 4096;
constexpr int kMaxNewTokens    = 1024;

// Largest offset <= end that sits on a UTF-8 character boundary, so a streamed
// chunk never splits a multi-byte char (a split makes QString::fromUtf8 emit the
// replacement char '?' — the CJK garbling bug).
size_t utf8Boundary(const std::string& s, size_t end) {
    while (end > 0 && end < s.size() && (static_cast<unsigned char>(s[end]) & 0xC0) == 0x80) --end;
    return end;
}

using ChatMsg = std::pair<std::string, std::string>;  // {role, content}, UTF-8

// Manual Gemma turns (no system role -> folded into the first user turn). Used as a
// fallback because llama_chat_apply_template can't parse the gemma4 arch template.
std::string gemmaPrompt(const std::vector<ChatMsg>& msgs) {
    std::string out, sys;
    for (const auto& [role, content] : msgs) {
        if (role == "system") { sys += content + "\n\n"; continue; }
        if (role == "user") { out += "<start_of_turn>user\n" + sys + content + "<end_of_turn>\n"; sys.clear(); }
        else                 { out += "<start_of_turn>model\n" + content + "<end_of_turn>\n"; }
    }
    return out + "<start_of_turn>model\n";
}

// Generic ChatML fallback (Qwen and many others understand it).
std::string chatmlPrompt(const std::vector<ChatMsg>& msgs) {
    std::string out;
    for (const auto& [role, content] : msgs)
        out += "<|im_start|>" + role + "\n" + content + "<|im_end|>\n";
    return out + "<|im_start|>assistant\n";
}

// Format the prompt for whatever model is loaded: prefer the GGUF's own chat
// template; if that isn't a template llama.cpp can render (e.g. gemma4), fall back
// to a hand-written format chosen by architecture. This is what makes the engine
// model-agnostic rather than Gemma-only.
std::string buildChatPrompt(llama_model* model, const std::vector<ChatMsg>& msgs) {
    std::vector<llama_chat_message> lc;
    lc.reserve(msgs.size());
    for (const auto& [role, content] : msgs) lc.push_back({ role.c_str(), content.c_str() });
    if (const char* tmpl = llama_model_chat_template(model, /*name=*/nullptr)) {
        std::vector<char> buf(8192);
        int need = llama_chat_apply_template(tmpl, lc.data(), lc.size(), /*add_ass=*/true,
                                             buf.data(), (int)buf.size());
        if (need > (int)buf.size()) {
            buf.resize(need);
            need = llama_chat_apply_template(tmpl, lc.data(), lc.size(), true, buf.data(), (int)buf.size());
        }
        if (need > 0) return std::string(buf.data(), need);
    }
    char arch[64] = {0};
    llama_model_meta_val_str(model, "general.architecture", arch, sizeof(arch));
    return std::strncmp(arch, "gemma", 5) == 0 ? gemmaPrompt(msgs) : chatmlPrompt(msgs);
}
} // namespace

// Model + backend live for the engine's lifetime (model load is costly); a fresh
// context is created per turn so each generation is stateless (full history is
// re-sent via the chat template, mirroring the SSE path).
struct LocalLmEngine::Impl {
    llama_model* model = nullptr;
    std::string  loadedModelPath;
    bool         backendInit = false;

    ~Impl() {
        if (model) llama_model_free(model);
        if (backendInit) llama_backend_free();
    }
};

LocalLmEngine::LocalLmEngine(QObject* parent)
    : QObject(parent), impl_(std::make_unique<Impl>()) {}

LocalLmEngine::~LocalLmEngine() = default;

bool LocalLmEngine::isAvailable() { return true; }

void LocalLmEngine::generate(const QString& modelPath, const QVariantList& history) {
    cancelled_ = false;

    // Collect the chat turns as UTF-8 {role, content}.
    std::vector<ChatMsg> msgs;
    msgs.emplace_back("system", kSystemPrompt);
    bool hasUser = false;
    for (const QVariant& v : history) {
        const QVariantMap m = v.toMap();
        const std::string content = m.value(QStringLiteral("content")).toString().toStdString();
        if (content.empty()) continue;
        const std::string role = m.value(QStringLiteral("role")).toString().toStdString();
        if (role == "user") hasUser = true;
        msgs.emplace_back(role, content);
    }
    if (!hasUser) { emit finished(); return; }

    std::thread([this, modelPath, msgs = std::move(msgs)]() mutable {
        if (!impl_->backendInit) { llama_backend_init(); impl_->backendInit = true; }

        // Lazily (re)load the model, reusing it while the path is stable.
        const std::string modelStd = modelPath.toStdString();
        if (!impl_->model || impl_->loadedModelPath != modelStd) {
            if (impl_->model) { llama_model_free(impl_->model); impl_->model = nullptr; }
            llama_model_params mp = llama_model_default_params();
            // Offload all layers when a GPU backend is present (e.g. a Vulkan build);
            // with a CPU-only llama build there are no GPU devices so it stays on CPU.
            mp.n_gpu_layers = 999;
            impl_->model = llama_model_load_from_file(modelStd.c_str(), mp);
            if (!impl_->model) { emit failed(QStringLiteral("Failed to load model")); return; }
            impl_->loadedModelPath = modelStd;
        }
        llama_model* model = impl_->model;
        const llama_vocab* vocab = llama_model_get_vocab(model);

        // Format for whatever model is loaded (its own chat template, or a per-arch
        // fallback). parse_special below tokenizes the resulting turn markers.
        const std::string prompt = buildChatPrompt(model, msgs);

        // Fresh context per turn (stateless).
        llama_context_params cp = llama_context_default_params();
        cp.n_ctx = kContextTokens;
        llama_context* ctx = llama_init_from_model(model, cp);
        if (!ctx) { emit failed(QStringLiteral("Failed to create context")); return; }

        // Tokenize (grow the buffer if the first pass reports it is too small).
        std::vector<llama_token> toks((int)prompt.size() + 64);
        int32_t nTok = llama_tokenize(vocab, prompt.c_str(), (int32_t)prompt.size(),
                                      toks.data(), (int32_t)toks.size(),
                                      /*add_special=*/true, /*parse_special=*/true);
        if (nTok < 0) {
            toks.resize(-nTok);
            nTok = llama_tokenize(vocab, prompt.c_str(), (int32_t)prompt.size(),
                                  toks.data(), (int32_t)toks.size(), true, true);
        }
        if (nTok <= 0) { llama_free(ctx); emit failed(QStringLiteral("Failed to tokenize")); return; }
        toks.resize(nTok);

        // Sampler chain: top-k -> top-p -> temperature -> distribution.
        llama_sampler* smpl = llama_sampler_chain_init(llama_sampler_chain_default_params());
        llama_sampler_chain_add(smpl, llama_sampler_init_top_k(40));
        llama_sampler_chain_add(smpl, llama_sampler_init_top_p(0.95f, 1));
        llama_sampler_chain_add(smpl, llama_sampler_init_temp(0.8f));
        llama_sampler_chain_add(smpl, llama_sampler_init_dist(LLAMA_DEFAULT_SEED));

        // llama_vocab_is_eog (below) is the primary stop, but some GGUFs don't register
        // their turn terminator as an EOG token (e.g. this gemma4 build emits the *text*
        // "<end_of_turn>") and would run on into a hallucinated dialogue. Belt-and-suspenders:
        // also stop on the common turn markers across families, held back a tail so a
        // partial marker never streams out.
        static const char* const kStops[] = {
            "<end_of_turn>", "<start_of_turn>",  // Gemma
            "<|im_end|>", "<|im_start|>",        // ChatML / Qwen
            "<|eot_id|>",                        // Llama 3
            "<|end|>",                           // Phi
            "</s>",                              // Mistral / Llama 2
        };
        constexpr size_t kKeep = 15;  // >= longest stop string ("<start_of_turn>")
        std::string gen; size_t emitted = 0; bool stopped = false;

        bool ok = llama_decode(ctx, llama_batch_get_one(toks.data(), (int32_t)toks.size())) == 0;
        for (int i = 0; ok && i < kMaxNewTokens && !cancelled_; ++i) {
            llama_token id = llama_sampler_sample(smpl, ctx, -1);
            if (llama_vocab_is_eog(vocab, id)) break;  // catches <eos>
            char buf[256];
            const int32_t k = llama_token_to_piece(vocab, id, buf, sizeof(buf), 0, /*special=*/false);
            if (k > 0) gen.append(buf, k);
            size_t hit = std::string::npos;
            for (const char* s : kStops) { const size_t h = gen.find(s); if (h < hit) hit = h; }
            if (hit != std::string::npos) {
                if (hit > emitted && !cancelled_)
                    emit replyChunk(QString::fromUtf8(gen.data() + emitted, int(hit - emitted)));
                stopped = true;
                break;
            }
            if (gen.size() > emitted + kKeep) {   // stream all but a possibly-partial-marker tail
                const size_t upto = utf8Boundary(gen, gen.size() - kKeep);  // don't split a char
                if (upto > emitted && !cancelled_)
                    emit replyChunk(QString::fromUtf8(gen.data() + emitted, int(upto - emitted)));
                emitted = upto;
            }
            ok = llama_decode(ctx, llama_batch_get_one(&id, 1)) == 0;
        }
        if (!stopped && emitted < gen.size() && !cancelled_)   // flush the tail on natural end / eos
            emit replyChunk(QString::fromUtf8(gen.data() + emitted, int(gen.size() - emitted)));

        if (!ok && !cancelled_) emit failed(QStringLiteral("On-device generation failed"));
        else if (!cancelled_)   emit finished();

        llama_sampler_free(smpl);
        llama_free(ctx);
    }).detach();
}

void LocalLmEngine::cancel() { cancelled_ = true; }

#else // !MIROBODY_ONDEVICE_LLM

// ---------------------------------------------------------------------------
// Stub — keeps the default Qt build green without the llama.cpp SDK.
// ---------------------------------------------------------------------------
struct LocalLmEngine::Impl {};

LocalLmEngine::LocalLmEngine(QObject* parent) : QObject(parent) {}
LocalLmEngine::~LocalLmEngine() = default;

bool LocalLmEngine::isAvailable() { return false; }

void LocalLmEngine::generate(const QString& modelPath, const QVariantList& history) {
    Q_UNUSED(modelPath);
    Q_UNUSED(history);
    emit failed(QStringLiteral("On-device model support is not built into this app."));
}

void LocalLmEngine::cancel() { cancelled_ = true; }

#endif

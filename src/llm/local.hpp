#pragma once

// On-device LLM client: llama.cpp + a GGUF model, no network.
//
// Deliberately the same `ainvoke(messages, system_prompt, on_event, user)` shape
// as the four cloud clients in this directory, so it can be held behind
// llm::Client via make_client<LocalClient>() and dropped into the agent
// framework with no adapter of its own. Nothing above the client needs to know a
// turn ran locally.
//
// Built only when configured with -DMIROBODY_ONDEVICE_LLM=ON and pointed at a
// llama.cpp SDK via -DLLAMA_CPP_DIR=<dir with include/llama.h + a built lib>.
// Otherwise a stub compiles in: available() is false and ainvoke() reports a
// single Error event — the same graceful degradation the repo uses for the
// absent JNI .so and the Qt on-device path.
//
// Threading: ainvoke() blocks the calling thread for the whole turn (a decode
// loop, seconds to minutes). The caller owns threading — a NAPI worker on
// HarmonyOS, a QThread on Qt. cancel() is safe from another thread.
//
// Lifetime: the model is loaded once, lazily on the first ainvoke(), and kept
// for the object's life; a multi-GB reload per turn would be unusable. Each turn
// gets a fresh context, so turns are stateless and history is replayed from
// `messages` — matching how the cloud clients behave.

#include "llm/event.hpp"
#include "llm/mirothinker.hpp"   // ChatMessage, EventHandler, UserContext

#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace mirobody { namespace llm {

struct LocalOptions {
    std::string model_path;      // absolute path to a .gguf
    /**
     * Ceiling for the context window. The turn takes the largest size that is both
     * within what the model was TRAINED for and what actually allocates, walking a
     * descending ladder from here.
     *
     * Chosen by allocation rather than arithmetic because the arithmetic is not
     * available: head_dim is not derivable (Qwen3.5 reports key_length 256 against an
     * n_embd/n_head of 160), and the layer discount a hybrid attention/SSM model earns
     * — only every full_attention_interval-th layer holds a KV cache — has no portable
     * metadata key. An estimate would be wrong in both directions; whether the buffer
     * allocates is a fact. It also adapts to the device for free: a 2B (cheap KV) takes
     * the top rung where a 4B on a small phone quietly settles lower.
     */
    int n_ctx_max    = 32768;
    /**
     * Worker threads. NOT hardware_concurrency(): measured on a Kirin 9020 (which
     * reports 12), the all-cores default was the WORST of every count tried —
     * 6 threads decoded 35% faster (924ms -> 682ms for 17 tokens). A three-cluster
     * phone SoC punishes handing the little cores a share of a matmul, because the
     * batch waits on the slowest thread. 6 also leaves cores for the UI rather than
     * saturating the device. <= 0 falls back to hardware_concurrency().
     */
    int n_threads    = 6;
    /**
     * Layers to offload to the GPU. 999 means "all", which llama.cpp clamps to the
     * model's real layer count; it also falls back to CPU when no GPU backend is
     * registered, so an over-large value is safe.
     *
     * Defaults to offloading only when the Vulkan backend was actually linked in
     * (the SDK carried libggml-vulkan.a). A CPU-only build asks for nothing rather
     * than relying on that fallback, so the intent is visible in the binary.
     */
#ifdef MIROBODY_ONDEVICE_LLM_VULKAN
    int n_gpu_layers = 999;
#else
    int n_gpu_layers = 0;
#endif
    /**
     * Optional extra ceiling on tokens per reply; <= 0 means "only the context
     * limits it", which is the default.
     *
     * A client is not a server: nothing is billed, no tenant needs fairness, and Stop
     * takes effect within one token. So this is NOT a policy cap — 2048 was, and it
     * cut a reasoning model off mid-thought.
     *
     * It is a backstop, set far above any real reply. "The user can stop it" assumes
     * the user is watching, and a runaway does happen (greedy sampling produced a
     * literal forever-loop before the sampler was fixed). Unbounded, that means the
     * whole context — at ~11 tok/s, close to an hour of a phone heating up in someone's
     * pocket. 8192 tokens is more than any genuine answer and bounds the damage to
     * minutes, so it never truncates real output but does end a loop.
     *
     * <= 0 removes even this; the context bound in ainvoke() always remains, since
     * every token appends to the KV cache and n_ctx - prompt is physics, not policy.
     */
    int n_predict    = 8192;
    /**
     * Tokens always kept free for the answer. History is trimmed until the prompt
     * fits in `n_ctx - n_reply_reserve`, so a long conversation can never squeeze the
     * reply down to nothing.
     *
     * This is what makes multi-turn work at all: every turn replays the whole
     * conversation (turns are stateless), so without trimming the prompt grows until
     * it fills the window — and raising n_predict cannot fix that, it only moves
     * where the wall is.
     */
    int n_reply_reserve = 1024;
    /**
     * Whether the model should reason before answering: 1 on, 0 off, -1 leave it to
     * the model.
     *
     * Explicit because otherwise it is an ACCIDENT. The Jinja template decides this
     * with an `enable_thinking` conditional, but llama_chat_apply_template has no
     * Jinja engine — it maps to the built-in CHATML form, which emits neither branch.
     * The model then falls back to whatever its training made default, and that
     * differs across one generation: Qwen3 reasons, Qwen3.5 does not (it inverted the
     * conditional), so the same code produced reasoning on one and an empty <think>
     * pair on the other with nothing in our control changing.
     *
     * On costs real time — the reasoning pass is decoded at the same tok/s as the
     * answer — which is why it is a knob and not a constant.
     *
     * Defaults OFF, matching what Qwen3.5's own template chose. Turning it on was
     * tried and was worse at this size: asked which ten sights make up 西湖十景, a 2B
     * does not reliably know, and with reasoning enabled it visibly circled — listing
     * the set, doubting it, listing another — instead of answering once. Qwen INVERTED
     * this default between Qwen3 and Qwen3.5, presumably having found the same thing,
     * and overriding a model author's judgement needs better evidence than a hunch.
     * Worth revisiting per model: a 4B may well earn it.
     */
    int thinking = 0;

    //-- sampling ---------------------------------------------------------------
    /**
     * Greedy decoding (temperature 0) is the textbook way to get a repetition loop,
     * and it did: a 2B answered "春季花港观鱼" forever. It was chosen for
     * reproducible benchmarking and then left in as the product sampler, which is the
     * actual cause — not the absence of a token cap.
     *
     * temperature <= 0 selects greedy, kept deliberately so a benchmark can still ask
     * for determinism.
     */
    float temperature    = 0.7f;
    float top_p          = 0.8f;
    int   top_k          = 20;
    /// 1.0 disables. Penalizing recent tokens is what actually breaks a loop.
    float repeat_penalty = 1.1f;
    int   repeat_last_n  = 256;
    /**
     * Presence penalty; 0 disables. Aimed at a different failure than repeat_penalty:
     * not the same tokens back to back, but the same SUBJECT raised over and over — a
     * model relisting a set it is unsure of. Exposed but off by default, because it
     * discourages any token reuse at all and CJK reuses characters constantly, so a
     * guessed value could quietly degrade ordinary prose. Turn it on with a measured
     * value, not a hunch.
     */
    float presence_penalty = 0.0f;
};

// What the last turn cost, for the probe UI and for benchmarking backends
// against each other. Zeroed at the start of every ainvoke().
struct LocalStats {
    double load_ms       = 0;    // model load; non-zero only on the first turn
    double prefill_ms    = 0;
    double decode_ms     = 0;
    int    prompt_tokens = 0;
    int    decoded_tokens = 0;
    int    n_threads     = 0;   // what the turn actually ran with
    int    n_ctx         = 0;   // context this turn got (see LocalOptions::n_ctx_max)
    double ctx_ms        = 0;   // building it; grows with n_ctx, so worth watching
    /// Oldest messages shed to make the prompt fit; >0 means the window slid.
    int    dropped_msgs  = 0;
    /**
     * How many tokens ended mid-character, so their trailing bytes had to wait for
     * the next token. Non-zero proves the byte-level-BPE split is real on this
     * model+language pair rather than a theoretical concern — which is the whole
     * reason the carry buffer exists.
     */
    int    utf8_splits   = 0;
};

/**
 * CPU features, reported as TWO separate things — conflating them is a trap.
 *
 *  has_*   what the silicon supports, read from AT_HWCAP at runtime. The ground
 *          truth for choosing build flags. Read directly rather than through
 *          ggml_cpu_has_*(), which despite the name are compile-time
 *          `__ARM_FEATURE_*` checks and therefore answer the other question.
 *          Also the only route on HarmonyOS, which denies an app /proc/cpuinfo.
 *
 *  built_* what THIS binary was compiled to emit. A cross build with no -march
 *          flags silently lands on baseline armv8-a, because CMake's feature
 *          probes cannot run target binaries and all fail.
 *
 * has_x && !built_x is money left on the table. For arm64 Q4 inference the two
 * that matter are dotprod (ARMv8.2 optional, present from Cortex-A76/A55 on) and
 * i8mm (ARMv8.6, so ARMv9 cores only).
 */
struct LocalCpuFeatures {
    // Silicon (AT_HWCAP / AT_HWCAP2).
    bool has_fp16    = false;   // FEAT_FP16      — asimdhp
    bool has_dotprod = false;   // FEAT_DotProd   — asimddp
    bool has_i8mm    = false;   // FEAT_I8MM
    bool has_bf16    = false;   // FEAT_BF16
    bool has_sve     = false;
    bool has_sve2    = false;
    bool has_sme     = false;

    // This build (ggml's compile-time view). All false when `variant` is set: with
    // runtime dispatch the CPU kernels live in a module this binary never linked, so
    // ggml_cpu_has_*() is not callable and, more to the point, no longer means
    // anything — there is no single set of kernels the build was compiled for.
    bool built_neon    = false;
    bool built_fma     = false;
    bool built_fp16    = false;
    bool built_dotprod = false;
    bool built_i8mm    = false;
    bool built_sve     = false;
    bool built_sme     = false;

    /**
     * Which CPU backend actually got loaded, e.g. "android_armv8.6_1" — the answer
     * that replaces the built_* flags in a GGML_CPU_ALL_VARIANTS build.
     *
     * Empty in a statically linked build (there was nothing to choose) and, more
     * usefully, empty when dispatch was configured but the modules were not found —
     * which is a shipping mistake that otherwise shows up only as slow inference.
     */
    std::string variant;
};

/**
 * Length of the longest prefix of `bytes` containing only COMPLETE UTF-8 sequences.
 *
 * A token is a byte sequence, not a character: Qwen and friends use byte-level BPE,
 * so one CJK character (3 bytes in UTF-8) is routinely split across two tokens.
 * Forwarding a token's bytes straight on therefore ships half a character, which
 * renders as a replacement glyph — and once the host has substituted U+FFFD, the
 * arrival of the remaining bytes cannot repair it. The caller keeps the incomplete
 * tail and prepends it to the next token.
 *
 * Free function so it is testable on its own; see tests/llm/think_splitter_test.cpp.
 */
size_t utf8_complete_len(const std::string& bytes);

/**
 * Splits a raw on-device token stream into answer text and `<think>` reasoning.
 *
 * The cloud clients receive reasoning as its own event because the wire protocol
 * separates it (`reasoning_content`, a `thinking` part…). llama.cpp hands back only
 * raw text, so a reasoning model's markers arrive inline — Qwen3 opens every reply
 * with `<think>…</think>`, and even in non-thinking mode emits the empty pair — and
 * without this they render literally in the bubble.
 *
 * Incremental on purpose: a tag is NOT delivered atomically. `<think>` can arrive as
 * `<` + `think` + `>` or `<th` + `ink>`, so anything that scans a whole string would
 * miss it. `feed()` holds back only the longest suffix that could still become a tag
 * and emits the rest immediately, which keeps streaming responsive.
 *
 * Compiled unconditionally (no llama.cpp dependency) so it is unit-testable in the
 * default build — see tests/llm/think_splitter_test.cpp.
 */
class ThinkSplitter {
public:
    /// Called per piece; `thinking` says which stream it belongs to. Return false to
    /// abort, which feed()/flush() propagate.
    using Emit = std::function<bool(bool thinking, const std::string& text)>;

    /// Consume one chunk of raw model text.
    bool feed(const std::string& chunk, const Emit& emit);

    /// Release anything still held at end of stream. A partial tag never completed,
    /// so it was content after all.
    bool flush(const Emit& emit);

    /// True while inside an unterminated `<think>`.
    bool thinking() const { return in_think_; }

private:
    std::string buf_;
    bool in_think_ = false;
};

class LocalClient {
public:
    /// True when compiled against llama.cpp (MIROBODY_ONDEVICE_LLM).
    static bool available();

    /**
     * Directory to dlopen ggml's backend modules from. Call once, before anything
     * else on this class; a no-op afterwards, and in a statically linked build.
     *
     * Needed because ggml's own search looks beside the executable, which on Android
     * is /system/bin/app_process — never where an app's libraries are. The right
     * argument there is ApplicationInfo.nativeLibraryDir, and only Java knows it.
     * That directory also has to hold real files, so the APK must not keep its
     * libraries compressed (android/app/build.gradle.kts).
     */
    static void backendPath(const std::string& dir);

    /// What this build was COMPILED to offload to: "cpu", "vulkan", or "none".
    static const char* backend();

    /**
     * The compute devices ggml actually registered at RUNTIME, as
     * "name(type)" joined by ", " — e.g. "Maleoon(igpu), CPU(cpu)".
     *
     * Necessary because backend() is a compile-time answer and llama.cpp falls back
     * to CPU in silence when a GPU backend initializes but finds no usable device.
     * Trusting backend() would then report a Vulkan run that never touched the GPU —
     * the same trap as reading ggml_cpu_has_*() as device capability. If no GPU or
     * iGPU appears here, an offload measurement is really a CPU measurement.
     */
    static std::string devices();

    /// Runtime CPU features of THIS device. All false in a stub build.
    static LocalCpuFeatures cpuFeatures();

    explicit LocalClient(LocalOptions opt);
    ~LocalClient();

    LocalClient(const LocalClient&)            = delete;
    LocalClient& operator=(const LocalClient&) = delete;

    /// Stream a reply. Emits EventType::Reply deltas and, on failure, a single
    /// EventType::Error. Returns true when the turn completed; false on error or
    /// when `on_event` returned false (the handler aborting is not an error).
    /// `user` is accepted for interface parity and ignored: a local turn runs no
    /// tools yet, so there is no identity to forward.
    bool ainvoke(const std::vector<ChatMessage>& messages,
                 const std::string&              system_prompt,
                 const EventHandler&             on_event,
                 const UserContext&              user = UserContext());

    /// Ask the in-flight turn to stop. Safe from another thread; a no-op when idle.
    void cancel();

    /**
     * Threads for subsequent turns; <= 0 means hardware_concurrency(). Takes effect
     * on the next turn without reloading the model, because the count is applied
     * when a turn builds its context, not when the model is read.
     *
     * Worth tuning rather than defaulting: on a big.LITTLE phone, handing the
     * little cores a share of a matmul can be a net loss — they finish late and the
     * whole batch waits on them.
     */
    void setThreads(int n);

    /// True once the model is resident (i.e. after a successful first turn).
    bool loaded() const;

    /**
     * Read the weights now, so the first turn does not pay for them.
     *
     * The load is the dominant cost of an on-device turn (seconds, a few GB off disk)
     * and it cannot be made cheaper — only moved somewhere nobody is waiting. A UI that
     * knows which model the user picked can call this while they are still typing.
     *
     * Idempotent; returns false with `err` set if the file will not load. A no-op
     * returning false in the stub build.
     */
    bool load(std::string& err);

    LocalStats stats() const;

    const LocalOptions& options() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}}

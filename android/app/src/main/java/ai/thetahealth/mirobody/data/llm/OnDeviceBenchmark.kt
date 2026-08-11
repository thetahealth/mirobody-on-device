package ai.thetahealth.mirobody.data.llm

import ai.thetahealth.mirobody.NativeBridge
import com.google.ai.edge.litertlm.Backend
import com.google.ai.edge.litertlm.ExperimentalApi
import com.google.ai.edge.litertlm.benchmark
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.withContext
import org.json.JSONObject

/**
 * One measured run of a model on one backend.
 *
 * Every number here comes from LiteRT-LM's own `benchmark()`, not from timing the chat
 * path — which is the point. Wrapping the streaming API and counting characters would
 * have measured our delta-diffing and Compose recomposition as much as the model, and it
 * has no idea how many TOKENS went by. The SDK reports prefill and decode separately, in
 * tokens, which is the only way to see the thing worth seeing: prefill and decode are
 * bound by different resources, so an accelerator can win one and lose the other.
 */
data class BenchmarkRun(
    val runtime: OnDeviceRuntime,
    val modelId: String,
    val displayName: String,
    val backend: OnDeviceBackend,
    val initSeconds: Double,
    val timeToFirstTokenSeconds: Double,
    val prefillTokens: Int,
    val decodeTokens: Int,
    val prefillTokensPerSecond: Double,
    val decodeTokensPerSecond: Double,
)

/**
 * Runs the SDK benchmark for [spec] and returns the numbers, or a failure.
 *
 * Blocking and slow (it loads the whole model), hence `Dispatchers.Default` and a caller
 * that shows progress. Failures are returned rather than thrown because "this backend
 * does not work on this device" is a RESULT here — a GPU build on a phone whose driver
 * has no usable OpenCL is exactly what the probe exists to find out, and it should read
 * as a row saying so, not as a crash.
 */
/**
 * Benchmark [spec] on whichever engine owns it, returning one comparable shape.
 *
 * The whole point is that the two runtimes report the same four things — load, time to
 * first token, prefill tok/s, decode tok/s — so a `.litertlm` and a `.gguf` of the same
 * model can be put side by side. LiteRT-LM has `benchmark()`; llama.cpp's numbers come
 * from `llm::LocalStats` over JNI, computed in C++ next to the fields they divide.
 */
suspend fun benchmarkOf(
    spec: OnDeviceModelSpec,
    modelPath: String,
    cacheDir: String,
): Result<BenchmarkRun> = when (spec.runtime) {
    OnDeviceRuntime.LITERT_LM -> runBenchmark(spec, modelPath, cacheDir)
    OnDeviceRuntime.LLAMA_CPP -> runLlamaBenchmark(spec, modelPath)
}

/**
 * llama.cpp via the JNI bridge.
 *
 * `Dispatchers.Default` and a fresh engine per call, for the same reasons the LiteRT path
 * uses them: the call blocks for the whole turn, and reusing a loaded model would hide
 * the load cost that is half of what is being measured.
 *
 * The prompt is the shared DEFAULT_PROMPT, so both runtimes prefill the same text — token
 * counts differ slightly because the tokenizers do, which is why the report carries the
 * counts alongside the rates.
 */
private suspend fun runLlamaBenchmark(
    spec: OnDeviceModelSpec,
    modelPath: String,
    decodeTokens: Int = DEFAULT_DECODE_TOKENS,
    prompt: String = DEFAULT_PROMPT,
): Result<BenchmarkRun> = withContext(Dispatchers.Default) {
    runCatching {
        // threads = 0 keeps LocalOptions' measured default (6), which beat every other
        // count on a three-cluster phone SoC — see its comment in src/llm/local.hpp.
        val json = JSONObject(NativeBridge().localBenchmark(modelPath, prompt, decodeTokens, 0))
        if (!json.optBoolean("ok")) {
            error(json.optString("error").ifBlank { "llama.cpp benchmark failed" })
        }
        val loadS = json.optDouble("loadMs", 0.0) / 1000.0
        val prefillS = json.optDouble("prefillMs", 0.0) / 1000.0
        BenchmarkRun(
            runtime = spec.runtime,
            modelId = spec.id,
            displayName = spec.displayName,
            backend = spec.backend,
            initSeconds = loadS,
            // LocalStats has no explicit TTFT: prefill is exactly the work before the
            // first token, so load + prefill IS the wait, and reporting it that way
            // keeps the column meaning the same thing on both engines.
            timeToFirstTokenSeconds = prefillS,
            prefillTokens = json.optInt("promptTokens"),
            decodeTokens = json.optInt("decodedTokens"),
            prefillTokensPerSecond = json.optDouble("prefillTps", 0.0),
            decodeTokensPerSecond = json.optDouble("decodeTps", 0.0),
        )
    }
}

@OptIn(ExperimentalApi::class)
suspend fun runBenchmark(
    spec: OnDeviceModelSpec,
    modelPath: String,
    cacheDir: String,
    prefillTokens: Int = DEFAULT_PREFILL_TOKENS,
    decodeTokens: Int = DEFAULT_DECODE_TOKENS,
    prompt: String = DEFAULT_PROMPT,
): Result<BenchmarkRun> = withContext(Dispatchers.Default) {
    // benchmark() is @ExperimentalApi in litertlm 0.15 — a measurement entry point, not
    // something the chat path depends on, so an opt-in scoped to this call is the whole
    // exposure. If it changes shape, only this file moves.
    runCatching {
        val info = benchmark(
            modelPath = modelPath,
            backend = when (spec.backend) {
                OnDeviceBackend.CPU -> Backend.CPU()
                OnDeviceBackend.GPU -> Backend.GPU()
            },
            prefillTokens = prefillTokens,
            decodeTokens = decodeTokens,
            cacheDir = cacheDir,
            prompt = prompt,
        )
        BenchmarkRun(
            runtime = spec.runtime,
            modelId = spec.id,
            displayName = spec.displayName,
            backend = spec.backend,
            initSeconds = info.initTimeInSecond,
            timeToFirstTokenSeconds = info.timeToFirstTokenInSecond,
            prefillTokens = info.lastPrefillTokenCount,
            decodeTokens = info.lastDecodeTokenCount,
            prefillTokensPerSecond = info.lastPrefillTokensPerSecond,
            decodeTokensPerSecond = info.lastDecodeTokensPerSecond,
        )
    }
}

// Fixed on purpose: two runs are only comparable if they did the same work. Small enough
// that a probe is seconds rather than minutes, large enough that decode reaches a steady
// rate instead of measuring the first token twice.
private const val DEFAULT_PREFILL_TOKENS = 128
private const val DEFAULT_DECODE_TOKENS = 64
/**
 * Long enough to prefill ~128 tokens, because that is what LiteRT-LM's benchmark() is
 * told to prefill and a rate measured over 29 tokens is not comparable to one measured
 * over 128 — the per-call overhead is amortised over four times fewer tokens.
 *
 * The two tokenizers will not agree exactly, which is why the report carries the token
 * COUNT beside every rate: check they are close before comparing the rates.
 */
private const val DEFAULT_PROMPT =
    "Explain in a short paragraph why resting heart rate tends to fall with regular " +
    "aerobic exercise. Cover the main adaptations: increased stroke volume, greater " +
    "vagal tone, reduced sympathetic drive, and plasma volume expansion. Mention how " +
    "quickly these changes appear in an untrained adult who begins training, roughly " +
    "how large the effect is in beats per minute, and which of them persists after a " +
    "few weeks of detraining. Keep the answer factual and avoid medical advice."

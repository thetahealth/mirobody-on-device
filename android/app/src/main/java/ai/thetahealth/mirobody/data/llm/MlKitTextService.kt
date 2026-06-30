package ai.thetahealth.mirobody.data.llm

import android.content.Context
import com.google.mlkit.genai.common.DownloadCallback
import com.google.mlkit.genai.common.FeatureStatus
import com.google.mlkit.genai.common.GenAiException
import com.google.mlkit.genai.rewriting.RewriterOptions
import com.google.mlkit.genai.rewriting.Rewriting
import com.google.mlkit.genai.rewriting.RewritingRequest
import com.google.mlkit.genai.summarization.SummarizationRequest
import com.google.mlkit.genai.summarization.Summarization
import com.google.mlkit.genai.summarization.SummarizerOptions
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.guava.await
import kotlinx.coroutines.suspendCancellableCoroutine
import kotlinx.coroutines.withContext
import kotlin.coroutines.resume
import kotlin.coroutines.resumeWithException

/**
 * On-device GenAI utilities backed by Gemini Nano (AICore) via ML Kit GenAI. This is
 * the "layered" cheap-task path: Nano can't do open-ended chat (that's Gemma 4 via
 * [LiteRtLlmEngine]) but it is fast and free for the bounded tasks it supports —
 * rewriting/proofreading a draft and summarizing text.
 *
 * Availability is per-device: ML Kit reports [FeatureStatus.UNAVAILABLE] on hardware
 * without AICore (most non-Pixel/Galaxy devices), in which case callers hide the
 * affordance. The model is downloaded on first use via [FeatureStatus.DOWNLOADABLE].
 */
class MlKitTextService(context: Context) {

    private val appContext = context.applicationContext

    /** Rewrite tone, mapped to ML Kit [RewriterOptions.OutputType]. */
    enum class Tone(val outputType: Int) {
        Rephrase(RewriterOptions.OutputType.REPHRASE),
        Shorten(RewriterOptions.OutputType.SHORTEN),
        Professional(RewriterOptions.OutputType.PROFESSIONAL),
        Friendly(RewriterOptions.OutputType.FRIENDLY),
    }

    /** Whether this device can rewrite on-device at all (AICore present). Cheap status check. */
    suspend fun isRewriteSupported(): Boolean = withContext(Dispatchers.IO) {
        runCatching {
            val client = Rewriting.getClient(rewriterOptions(Tone.Rephrase, "en"))
            try {
                client.checkFeatureStatus().await() != FeatureStatus.UNAVAILABLE
            } finally {
                client.close()
            }
        }.getOrDefault(false)
    }

    /**
     * Rewrite [text] in the given [tone] on-device, downloading the feature first if
     * needed. Returns the rewritten text, or a failure if the device is unsupported or
     * inference fails (callers fall back to the original draft).
     */
    suspend fun polish(text: String, tone: Tone, language: String): Result<String> =
        withContext(Dispatchers.IO) {
            val client = Rewriting.getClient(rewriterOptions(tone, language))
            try {
                if (!ensureReady(
                        status = { client.checkFeatureStatus().await() },
                        download = { cb -> client.downloadFeature(cb) },
                    )
                ) {
                    return@withContext Result.failure(
                        IllegalStateException("On-device rewriting is not available on this device"),
                    )
                }
                val request = RewritingRequest.builder(text).build()
                val out = StringBuilder()
                // The streaming callback delivers incremental text; accumulate to the full result.
                client.runInference(request) { newText -> out.append(newText) }.await()
                Result.success(out.toString().ifBlank { text })
            } catch (t: Throwable) {
                Result.failure(t)
            } finally {
                client.close()
            }
        }

    /** Summarize [text] into bullet points on-device. */
    suspend fun summarize(text: String, language: String): Result<String> =
        withContext(Dispatchers.IO) {
            val client = Summarization.getClient(summarizerOptions(language))
            try {
                if (!ensureReady(
                        status = { client.checkFeatureStatus().await() },
                        download = { cb -> client.downloadFeature(cb) },
                    )
                ) {
                    return@withContext Result.failure(
                        IllegalStateException("On-device summarization is not available on this device"),
                    )
                }
                val request = SummarizationRequest.builder(text).build()
                val out = StringBuilder()
                client.runInference(request) { newText -> out.append(newText) }.await()
                Result.success(out.toString())
            } catch (t: Throwable) {
                Result.failure(t)
            } finally {
                client.close()
            }
        }

    /**
     * Resolve [FeatureStatus] to readiness, downloading (and awaiting) the Nano feature
     * when it's downloadable. Returns false when the device can't run it at all.
     */
    private suspend fun ensureReady(
        status: suspend () -> Int,
        download: (DownloadCallback) -> Unit,
    ): Boolean = when (status()) {
        FeatureStatus.AVAILABLE -> true
        FeatureStatus.UNAVAILABLE -> false
        else -> {
            // DOWNLOADABLE / DOWNLOADING: kick off (or join) the download and wait.
            suspendCancellableCoroutine { cont ->
                download(object : DownloadCallback {
                    override fun onDownloadStarted(bytesToDownload: Long) = Unit
                    override fun onDownloadProgress(totalBytesDownloaded: Long) = Unit
                    override fun onDownloadCompleted() {
                        if (cont.isActive) cont.resume(Unit)
                    }
                    override fun onDownloadFailed(e: GenAiException) {
                        if (cont.isActive) cont.resumeWithException(e)
                    }
                })
            }
            status() == FeatureStatus.AVAILABLE
        }
    }

    private fun rewriterOptions(tone: Tone, language: String) =
        RewriterOptions.builder(appContext)
            .setOutputType(tone.outputType)
            .setLanguage(rewriteLanguage(language))
            .build()

    private fun summarizerOptions(language: String) =
        SummarizerOptions.builder(appContext)
            .setInputType(SummarizerOptions.InputType.CONVERSATION)
            .setOutputType(SummarizerOptions.OutputType.THREE_BULLETS)
            .setLanguage(summarizeLanguage(language))
            .build()

    // ML Kit rewriting supports EN/JA/FR/DE/IT/ES/KO; everything else falls back to English.
    private fun rewriteLanguage(code: String): Int = when (code) {
        "ja" -> RewriterOptions.Language.JAPANESE
        "fr" -> RewriterOptions.Language.FRENCH
        "de" -> RewriterOptions.Language.GERMAN
        "es" -> RewriterOptions.Language.SPANISH
        "ko" -> RewriterOptions.Language.KOREAN
        else -> RewriterOptions.Language.ENGLISH
    }

    // ML Kit summarization supports EN/JA/KO; everything else falls back to English.
    private fun summarizeLanguage(code: String): Int = when (code) {
        "ja" -> SummarizerOptions.Language.JAPANESE
        "ko" -> SummarizerOptions.Language.KOREAN
        else -> SummarizerOptions.Language.ENGLISH
    }
}

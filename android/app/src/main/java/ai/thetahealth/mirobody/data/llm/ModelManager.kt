package ai.thetahealth.mirobody.data.llm

import android.content.Context
import java.io.File
import java.util.concurrent.TimeUnit
import java.util.concurrent.atomic.AtomicBoolean
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.ensureActive
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.flow.asStateFlow
import kotlinx.coroutines.withContext
import okhttp3.OkHttpClient
import okhttp3.Request
import kotlin.coroutines.coroutineContext

/**
 * Owns the on-device model file: where it lives, whether it's present, and the
 * download (with resume + progress). Deliberately independent of the user's
 * configured server — the download goes straight to Hugging Face over its own
 * timeout-free [OkHttpClient], so it works regardless of the chat backend.
 *
 * NOTE: a ~2.5 GB download should ultimately run under a foreground service /
 * WorkManager to survive process death. This v1 runs it from the caller's
 * coroutine scope and supports HTTP range-resume so an interrupted download
 * continues from the partial file rather than restarting.
 */
class ModelManager(context: Context) {

    private val appContext = context.applicationContext

    private val modelsDir: File by lazy {
        File(appContext.filesDir, "models").apply { mkdirs() }
    }

    /** Final model file once a download completes. */
    val modelFile: File get() = File(modelsDir, OnDeviceModel.FILE_NAME)

    /** Partial download target; promoted to [modelFile] on success. */
    private val partFile: File get() = File(modelsDir, OnDeviceModel.FILE_NAME + ".part")

    private val http: OkHttpClient by lazy {
        OkHttpClient.Builder()
            // A multi-GB transfer must not hit a read/call timeout mid-stream.
            .connectTimeout(30, TimeUnit.SECONDS)
            .readTimeout(0, TimeUnit.SECONDS)
            .writeTimeout(0, TimeUnit.SECONDS)
            .callTimeout(0, TimeUnit.SECONDS)
            .build()
    }

    private val _status = MutableStateFlow<OnDeviceModelStatus>(
        if (modelFile.exists() && modelFile.length() > 0) OnDeviceModelStatus.Ready
        else OnDeviceModelStatus.Absent,
    )
    val status: StateFlow<OnDeviceModelStatus> = _status.asStateFlow()

    private val downloading = AtomicBoolean(false)

    fun isReady(): Boolean = modelFile.exists() && modelFile.length() > 0

    /**
     * Download the model to private storage, resuming a prior partial file when present.
     * Updates [status] as it goes. Cancelling the calling coroutine pauses the download
     * (the `.part` file is kept for resume). Returns true on success.
     */
    suspend fun download(): Boolean {
        if (isReady()) {
            _status.value = OnDeviceModelStatus.Ready
            return true
        }
        if (!downloading.compareAndSet(false, true)) return false
        try {
            return withContext(Dispatchers.IO) {
                runCatching { performDownload() }
                    .onFailure {
                        // A cancellation is a pause, not a failure: keep the .part file
                        // and report the partial-progress state, not an error.
                        if (it is kotlinx.coroutines.CancellationException) {
                            _status.value = OnDeviceModelStatus.Downloading(partFile.length(), -1)
                            throw it
                        }
                        _status.value = OnDeviceModelStatus.Failed(it.message ?: "Download failed")
                    }
                    .getOrDefault(false)
            }
        } finally {
            downloading.set(false)
        }
    }

    private suspend fun performDownload(): Boolean {
        val existing = if (partFile.exists()) partFile.length() else 0L
        val builder = Request.Builder().url(OnDeviceModel.DOWNLOAD_URL)
        if (existing > 0) builder.header("Range", "bytes=$existing-")

        _status.value = OnDeviceModelStatus.Downloading(existing, OnDeviceModel.APPROX_BYTES)

        http.newCall(builder.build()).execute().use { resp ->
            if (!resp.isSuccessful) {
                _status.value = OnDeviceModelStatus.Failed("HTTP ${resp.code}")
                return false
            }
            val body = resp.body ?: run {
                _status.value = OnDeviceModelStatus.Failed("Empty response")
                return false
            }
            // When the server honours the Range request (206) the body length is the
            // remainder; otherwise it restarts from zero, so reset the part file.
            val resumed = resp.code == 206
            val startAt = if (resumed) existing else 0L
            val total = (body.contentLength().takeIf { it > 0 }?.plus(startAt)) ?: OnDeviceModel.APPROX_BYTES

            val sink = if (resumed) java.io.FileOutputStream(partFile, /* append = */ true)
            else java.io.FileOutputStream(partFile, /* append = */ false)

            sink.use { out ->
                body.byteStream().use { input ->
                    val buf = ByteArray(1 shl 16)
                    var written = startAt
                    while (true) {
                        coroutineContext.ensureActive() // cooperative pause/cancel
                        val n = input.read(buf)
                        if (n < 0) break
                        out.write(buf, 0, n)
                        written += n
                        _status.value = OnDeviceModelStatus.Downloading(written, total)
                    }
                    out.flush()
                }
            }
        }

        if (!partFile.renameTo(modelFile)) {
            _status.value = OnDeviceModelStatus.Failed("Could not finalize model file")
            return false
        }
        _status.value = OnDeviceModelStatus.Ready
        return true
    }

    /** Remove the model (and any partial) to reclaim ~2.5 GB. */
    fun delete() {
        modelFile.delete()
        partFile.delete()
        _status.value = OnDeviceModelStatus.Absent
    }
}

package ai.thetahealth.mirobody.data.llm

import android.content.Context
import java.io.File
import java.util.concurrent.ConcurrentHashMap
import java.util.concurrent.TimeUnit
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.ensureActive
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.flow.asStateFlow
import kotlinx.coroutines.flow.update
import kotlinx.coroutines.withContext
import okhttp3.OkHttpClient
import okhttp3.Request
import kotlin.coroutines.coroutineContext

/**
 * Owns the on-device model files: where each lives, whether it's present, and the
 * download (with resume + progress) — for every model in [OnDeviceModel.CATALOG].
 * Deliberately independent of the user's configured server: downloads go straight to
 * Hugging Face over a timeout-free [OkHttpClient], so they work regardless of the chat
 * backend. Several models can coexist on disk; the user picks which to run.
 *
 * NOTE: a multi-GB download should ultimately run under a foreground service /
 * WorkManager to survive process death. This runs from the caller's coroutine scope
 * and supports HTTP range-resume so an interrupted download continues from the partial
 * file rather than restarting.
 */
class ModelManager(context: Context) {

    private val appContext = context.applicationContext

    private val modelsDir: File by lazy {
        File(appContext.filesDir, "models").apply { mkdirs() }
    }

    fun fileFor(spec: OnDeviceModelSpec): File = File(modelsDir, spec.fileName)
    private fun partFor(spec: OnDeviceModelSpec): File = File(modelsDir, spec.fileName + ".part")

    private val http: OkHttpClient by lazy {
        OkHttpClient.Builder()
            // A multi-GB transfer must not hit a read/call timeout mid-stream.
            .connectTimeout(30, TimeUnit.SECONDS)
            .readTimeout(0, TimeUnit.SECONDS)
            .writeTimeout(0, TimeUnit.SECONDS)
            .callTimeout(0, TimeUnit.SECONDS)
            .build()
    }

    /** Per-model status, keyed by [OnDeviceModelSpec.id]. Seeded from what's on disk. */
    private val _statuses = MutableStateFlow<Map<String, OnDeviceModelStatus>>(
        OnDeviceModel.CATALOG.associate { spec ->
            spec.id to if (isReady(spec)) OnDeviceModelStatus.Ready else OnDeviceModelStatus.Absent
        },
    )
    val statuses: StateFlow<Map<String, OnDeviceModelStatus>> = _statuses.asStateFlow()

    fun status(spec: OnDeviceModelSpec): OnDeviceModelStatus =
        _statuses.value[spec.id] ?: OnDeviceModelStatus.Absent

    private fun setStatus(spec: OnDeviceModelSpec, status: OnDeviceModelStatus) {
        _statuses.update { it + (spec.id to status) }
    }

    /** Ids currently in flight, so a repeated download() of the same model is a no-op. */
    private val downloading = ConcurrentHashMap.newKeySet<String>()

    fun isReady(spec: OnDeviceModelSpec): Boolean {
        val f = fileFor(spec)
        return f.exists() && f.length() > 0
    }

    /**
     * Download [spec] to private storage, resuming a prior partial file when present.
     * Updates [statuses] as it goes. Cancelling the calling coroutine pauses the
     * download (the `.part` file is kept for resume). Returns true on success.
     */
    suspend fun download(spec: OnDeviceModelSpec): Boolean {
        if (isReady(spec)) {
            setStatus(spec, OnDeviceModelStatus.Ready)
            return true
        }
        if (!downloading.add(spec.id)) return false
        try {
            return withContext(Dispatchers.IO) {
                runCatching { performDownload(spec) }
                    .onFailure {
                        // A cancellation is a pause, not a failure: keep the .part file
                        // and report the partial-progress state, not an error.
                        if (it is kotlinx.coroutines.CancellationException) {
                            setStatus(spec, OnDeviceModelStatus.Downloading(partFor(spec).length(), -1))
                            throw it
                        }
                        setStatus(spec, OnDeviceModelStatus.Failed(it.message ?: "Download failed"))
                    }
                    .getOrDefault(false)
            }
        } finally {
            downloading.remove(spec.id)
        }
    }

    private suspend fun performDownload(spec: OnDeviceModelSpec): Boolean {
        val partFile = partFor(spec)
        val existing = if (partFile.exists()) partFile.length() else 0L
        val builder = Request.Builder().url(spec.downloadUrl)
        if (existing > 0) builder.header("Range", "bytes=$existing-")

        setStatus(spec, OnDeviceModelStatus.Downloading(existing, spec.approxBytes))

        http.newCall(builder.build()).execute().use { resp ->
            if (!resp.isSuccessful) {
                setStatus(spec, OnDeviceModelStatus.Failed("HTTP ${resp.code}"))
                return false
            }
            val body = resp.body ?: run {
                setStatus(spec, OnDeviceModelStatus.Failed("Empty response"))
                return false
            }
            // When the server honours the Range request (206) the body length is the
            // remainder; otherwise it restarts from zero, so reset the part file.
            val resumed = resp.code == 206
            val startAt = if (resumed) existing else 0L
            val total = (body.contentLength().takeIf { it > 0 }?.plus(startAt)) ?: spec.approxBytes

            val sink = java.io.FileOutputStream(partFile, /* append = */ resumed)

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
                        setStatus(spec, OnDeviceModelStatus.Downloading(written, total))
                    }
                    out.flush()
                }
            }
        }

        if (!partFile.renameTo(fileFor(spec))) {
            setStatus(spec, OnDeviceModelStatus.Failed("Could not finalize model file"))
            return false
        }
        setStatus(spec, OnDeviceModelStatus.Ready)
        return true
    }

    /** Remove [spec] (and any partial) to reclaim its storage. */
    fun delete(spec: OnDeviceModelSpec) {
        fileFor(spec).delete()
        partFor(spec).delete()
        setStatus(spec, OnDeviceModelStatus.Absent)
    }
}

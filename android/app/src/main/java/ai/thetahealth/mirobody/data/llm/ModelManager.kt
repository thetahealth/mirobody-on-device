package ai.thetahealth.mirobody.data.llm

import android.content.Context
import android.net.Uri
import android.os.Build
import android.os.Environment
import android.provider.OpenableColumns
import java.io.File
import java.util.UUID
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
import org.json.JSONArray
import org.json.JSONObject
import kotlin.coroutines.coroutineContext

/**
 * Owns the on-device model files: where each lives, whether it's present, and the
 * download (with resume + progress) — for every model in [OnDeviceModel.CATALOG] —
 * plus user-imported models picked from the filesystem.
 *
 * Model files live in **shared external storage** (`<sdcard>/Mirobody/models`), NOT
 * app-private storage, so they SURVIVE an app uninstall/reinstall — a multi-GB download
 * shouldn't vanish just because the app was reinstalled during development. That storage
 * needs All-Files-Access (see [hasStorageAccess]); the app is source-distributed (not on
 * Play), so the broad permission is acceptable here.
 *
 * Two acquisition sources, mirroring the desktop clients:
 *  - **download** a catalog model from Hugging Face, or
 *  - **import** any existing model file from the device (referenced in place when a real
 *    path is available, else copied into the shared dir — either way outside app-private
 *    storage). See [import].
 *
 * NOTE: a multi-GB download should ultimately run under a foreground service /
 * WorkManager to survive process death. This runs from the caller's coroutine scope
 * and supports HTTP range-resume so an interrupted download continues from the partial
 * file rather than restarting.
 */
class ModelManager(context: Context) {

    private val appContext = context.applicationContext

    /** Shared, uninstall-surviving model dir. May not be writable until [hasStorageAccess]. */
    private val modelsDir: File
        get() = File(Environment.getExternalStorageDirectory(), "Mirobody/models")

    /** Old app-private location; models here are migrated to [modelsDir] on first access. */
    private val legacyDir: File
        get() = File(appContext.filesDir, "models")

    private val importPrefs = appContext.getSharedPreferences("ondevice_imports", Context.MODE_PRIVATE)

    // NOTE: the init block is deliberately NOT here. It calls loadImports(), which writes
    // _imported / _statuses, and Kotlin runs property initializers and init blocks in
    // declaration order -- from here those flows are still null. See below.

    fun fileFor(spec: OnDeviceModelSpec): File =
        spec.localPath?.let(::File) ?: File(modelsDir, spec.fileName)

    private fun partFor(spec: OnDeviceModelSpec): File = File(modelsDir, spec.fileName + ".part")

    /**
     * Whether the app can read/write the shared models dir. Downloads and (path-referenced)
     * imports need this; without it the manage UI prompts the user to grant it. On API < 30
     * the legacy storage permissions are requested instead, so this reports true there.
     */
    fun hasStorageAccess(): Boolean =
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.R) {
            Environment.isExternalStorageManager()
        } else {
            appContext.checkSelfPermission(android.Manifest.permission.WRITE_EXTERNAL_STORAGE) ==
                android.content.pm.PackageManager.PERMISSION_GRANTED
        }

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
    private val _statuses = MutableStateFlow<Map<String, OnDeviceModelStatus>>(emptyMap())
    val statuses: StateFlow<Map<String, OnDeviceModelStatus>> = _statuses.asStateFlow()

    /** User-imported models currently registered; drives the imported section of the picker. */
    private val _imported = MutableStateFlow<List<OnDeviceModelSpec>>(emptyList())
    val imported: StateFlow<List<OnDeviceModelSpec>> = _imported.asStateFlow()

    /**
     * Must stay BELOW _statuses / _imported: loadImports() assigns both, and Kotlin runs
     * property initializers and init blocks strictly in declaration order. Higher up (where
     * this used to sit, next to importPrefs) the flows are still null and every construction
     * of ModelManager dies with an NPE -- which meant the app could not start at all, since
     * AppContainer builds one eagerly.
     */
    init {
        migrateLegacyModels()
        loadImports()
    }

    fun status(spec: OnDeviceModelSpec): OnDeviceModelStatus =
        _statuses.value[spec.id] ?: OnDeviceModelStatus.Absent

    private fun setStatus(spec: OnDeviceModelSpec, status: OnDeviceModelStatus) {
        _statuses.update { it + (spec.id to status) }
    }

    private fun refreshCatalogStatuses() {
        _statuses.update { current ->
            val seeded = OnDeviceModel.CATALOG.associate { spec ->
                // Keep an in-flight download's live status; otherwise re-derive from disk.
                val existing = current[spec.id]
                if (existing is OnDeviceModelStatus.Downloading) spec.id to existing
                else spec.id to if (isReady(spec)) OnDeviceModelStatus.Ready else OnDeviceModelStatus.Absent
            }
            current + seeded
        }
    }

    /** Ids currently in flight, so a repeated download() of the same model is a no-op. */
    private val downloading = ConcurrentHashMap.newKeySet<String>()

    fun isReady(spec: OnDeviceModelSpec): Boolean {
        val f = fileFor(spec)
        return f.exists() && f.length() > 0
    }

    /**
     * Download [spec] to the shared models dir, resuming a prior partial file when present.
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
        modelsDir.mkdirs()
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

    /** Remove a catalog model (and any partial) to reclaim its storage. */
    fun delete(spec: OnDeviceModelSpec) {
        fileFor(spec).delete()
        partFor(spec).delete()
        setStatus(spec, OnDeviceModelStatus.Absent)
    }

    // ---- Import (a model file the user already has on the device) -----------------------

    /**
     * Import the model at [uri] (from the system file picker) as a new on-device model.
     * The file ends up OUTSIDE app-private storage: referenced in place when [uri] resolves
     * to a readable real path, otherwise copied into the shared models dir. Registers it so
     * it appears in the picker immediately. Returns the new spec, or null on failure.
     */
    suspend fun import(uri: Uri): OnDeviceModelSpec? = withContext(Dispatchers.IO) {
        val displayName = queryDisplayName(uri)
        val id = OnDeviceModel.IMPORTED_ID_PREFIX + UUID.randomUUID().toString().take(8)

        // Prefer referencing the original file in place (no multi-GB copy).
        val realPath = resolveRealPath(uri)?.takeIf { File(it).canRead() }
        val (path, size) = if (realPath != null) {
            realPath to File(realPath).length()
        } else {
            // Fall back to copying into the shared dir (still survives uninstall).
            modelsDir.mkdirs()
            val dest = File(modelsDir, sanitize(displayName))
            val copied = runCatching {
                appContext.contentResolver.openInputStream(uri)?.use { input ->
                    dest.outputStream().use { out -> input.copyTo(out, 1 shl 16) }
                } ?: return@withContext null
            }.isSuccess
            if (!copied || dest.length() <= 0) return@withContext null
            dest.absolutePath to dest.length()
        }

        val label = displayName.substringBeforeLast('.').ifBlank { "Imported model" }
        val spec = OnDeviceModel.importedSpec(id, label, path, size)
        OnDeviceModel.registerImported(spec)
        setStatus(spec, OnDeviceModelStatus.Ready)
        _imported.update { it + spec }
        persistImports()
        spec
    }

    /**
     * Forget an imported model. Its file is deleted only if it was COPIED into our shared
     * dir; a file referenced in place (the user's own path) is left untouched.
     */
    fun deleteImported(spec: OnDeviceModelSpec) {
        val path = spec.localPath ?: return
        val f = File(path)
        if (f.parentFile?.absolutePath == modelsDir.absolutePath) f.delete()
        OnDeviceModel.unregisterImported(spec.id)
        _statuses.update { it - spec.id }
        _imported.update { list -> list.filterNot { it.id == spec.id } }
        persistImports()
    }

    private fun queryDisplayName(uri: Uri): String {
        runCatching {
            appContext.contentResolver.query(uri, arrayOf(OpenableColumns.DISPLAY_NAME), null, null, null)
                ?.use { c -> if (c.moveToFirst()) return c.getString(0) }
        }
        return uri.lastPathSegment?.substringAfterLast('/') ?: "model.litertlm"
    }

    /** Best-effort content://→filesystem-path resolution for the common storage providers. */
    private fun resolveRealPath(uri: Uri): String? {
        if (uri.scheme == "file") return uri.path
        val docId = runCatching { android.provider.DocumentsContract.getDocumentId(uri) }.getOrNull()
            ?: return null
        // e.g. "primary:Download/gemma.litertlm" or "raw:/storage/emulated/0/…"
        return when {
            docId.startsWith("raw:") -> docId.removePrefix("raw:")
            docId.startsWith("primary:") ->
                File(Environment.getExternalStorageDirectory(), docId.removePrefix("primary:")).absolutePath
            else -> null
        }
    }

    private fun sanitize(name: String): String =
        name.replace(Regex("[^A-Za-z0-9._-]"), "_").ifBlank { "model.litertlm" }

    // ---- Persistence of the imported list ----------------------------------------------

    private fun loadImports() {
        val json = importPrefs.getString(KEY_IMPORTS, null)
        val specs = runCatching {
            val arr = JSONArray(json ?: "[]")
            (0 until arr.length()).mapNotNull { i ->
                val o = arr.getJSONObject(i)
                val path = o.getString("path")
                // Drop entries whose file the user has since removed.
                if (!File(path).let { it.exists() && it.length() > 0 }) return@mapNotNull null
                OnDeviceModel.importedSpec(
                    id = o.getString("id"),
                    displayName = o.getString("name"),
                    path = path,
                    sizeBytes = File(path).length(),
                )
            }
        }.getOrDefault(emptyList())

        specs.forEach { OnDeviceModel.registerImported(it) }
        _imported.value = specs
        _statuses.update { it + specs.associate { s -> s.id to OnDeviceModelStatus.Ready } }
        refreshCatalogStatuses()
    }

    private fun persistImports() {
        val arr = JSONArray()
        _imported.value.forEach { spec ->
            arr.put(
                JSONObject()
                    .put("id", spec.id)
                    .put("name", spec.displayName)
                    .put("path", spec.localPath),
            )
        }
        importPrefs.edit().putString(KEY_IMPORTS, arr.toString()).apply()
    }

    // ---- One-time migration off the old app-private dir --------------------------------

    private fun migrateLegacyModels() {
        val old = legacyDir
        if (!old.isDirectory) return
        if (!hasStorageAccess()) return // retry on a later launch once granted
        runCatching {
            modelsDir.mkdirs()
            old.listFiles()?.forEach { src ->
                val dest = File(modelsDir, src.name)
                if (!dest.exists() && src.isFile) src.copyTo(dest, overwrite = false)
                src.delete()
            }
            old.delete()
        }
    }

    private companion object {
        const val KEY_IMPORTS = "imports"
    }
}

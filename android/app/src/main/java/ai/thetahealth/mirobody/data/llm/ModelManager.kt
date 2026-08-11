package ai.thetahealth.mirobody.data.llm

import android.content.Context
import android.net.Uri
import android.os.Build
import android.os.Environment
import android.provider.OpenableColumns
import java.io.File
import java.security.MessageDigest
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
import okhttp3.Protocol
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
 * Both are checked against the catalog's published SHA-256 rather than trusted by name or
 * length, which is what makes "this model is already here" a statement about the bytes:
 * a download skips the network only on a digest match, a finished transfer is verified
 * before it is renamed into place, and an import that turns out to BE a catalog model is
 * adopted as that model instead of becoming a second copy of it.
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
     * Where LiteRT-LM may write its compiled-model cache for [spec] — app-private, and a
     * directory of its own per model.
     *
     * NOT beside the model, which is where it landed before. That cache is roughly the
     * SIZE OF THE MODEL (it is the weights re-laid-out for the chosen backend), so
     * putting it in the shared models dir doubled what the user sees, in a folder whose
     * whole promise is "your multi-GB download survives an uninstall". A derived file
     * that can be rebuilt from the model in one load has not earned that.
     *
     * `cacheDir` specifically: the OS may reclaim it under storage pressure, which for a
     * few GB of regenerable data is the correct outcome, and "clear cache" in system
     * settings then does what a user expects.
     *
     * A directory PER MODEL so removal needs no knowledge of what LiteRT names the files
     * inside — [delete] drops the whole tree. That is also what keeps two backends of the
     * same weights (the CPU and GPU twins) from sharing one cache.
     */
    fun cacheDirFor(spec: OnDeviceModelSpec): File =
        File(File(appContext.cacheDir, "litertlm"), spec.id).also { it.mkdirs() }

    /** Bytes currently held by every model cache, for the probe to report. */
    fun cacheBytes(): Long =
        File(appContext.cacheDir, "litertlm").walkBottomUp().filter { it.isFile }.sumOf { it.length() }

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
            // HTTP/1.1 on purpose. These are single 2-4 GB responses held open for
            // minutes, and that is the shape HTTP/2 handles worst: a multiplexed stream
            // carrying one enormous body gets reset by intermediaries and CDN edges long
            // before a plain connection would be dropped, surfacing as "connection
            // closed" partway through. There is nothing to multiplex here — one request,
            // one body — so the feature costs us and buys nothing.
            .protocols(listOf(Protocol.HTTP_1_1))
            .build()
    }

    /**
     * Hugging Face redirects an LFS/Xet file to a CDN, and a request with no User-Agent
     * is exactly the shape a CDN throttles or refuses. Naming ourselves is both polite
     * and the difference between a download that works and one that does not.
     */
    private fun requestFor(url: String, rangeFrom: Long): Request {
        val b = Request.Builder().url(url).header("User-Agent", USER_AGENT)
        if (rangeFrom > 0) b.header("Range", "bytes=$rangeFrom-")
        return b.build()
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
        // After loadImports(), so an imported model's own name is known and its cache is
        // recognised too.
        sweepStrayCaches()
    }

    fun status(spec: OnDeviceModelSpec): OnDeviceModelStatus =
        _statuses.value[spec.id] ?: OnDeviceModelStatus.Absent

    private fun setStatus(spec: OnDeviceModelSpec, status: OnDeviceModelStatus) {
        _statuses.update { it + (spec.id to status) }
    }

    private fun refreshCatalogStatuses() {
        _statuses.update { current ->
            // ALL, not CATALOG: the debug probe's GPU twin needs a status too.
            val seeded = OnDeviceModel.ALL.associate { spec ->
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

    /**
     * Is the model's file on disk and the right LENGTH?
     *
     * Length, not digest: this runs on every status refresh (each time the manage dialog
     * or the picker opens), and hashing a few GB there would stall the UI for seconds
     * each time. For a catalog entry `approxBytes` is Hugging Face's exact `lfs.size`, so
     * an exact match is free and already rejects the common leftover — a truncated
     * download. The digest is what [download] runs before it decides to skip the network;
     * see [matchesDigest].
     */
    fun isReady(spec: OnDeviceModelSpec): Boolean {
        val f = fileFor(spec)
        if (!f.exists()) return false
        // An import has no published size to compare against; presence is all we have.
        return if (spec.sha256.isEmpty()) f.length() > 0 else f.length() == spec.approxBytes
    }

    /**
     * Does the file at [file] hash to [OnDeviceModelSpec.sha256]?
     *
     * SHA-256 rather than MD5 because it is the digest Hugging Face actually publishes
     * (the LFS `oid`); an MD5 would have to be produced by downloading every model and
     * hashing it by hand, and would then be a number nobody could re-derive from the
     * source of truth. Specs with no digest (imports) are taken on trust.
     */
    private suspend fun matchesDigest(spec: OnDeviceModelSpec, file: File): Boolean {
        if (spec.sha256.isEmpty()) return true
        return withContext(Dispatchers.IO) {
            val md = MessageDigest.getInstance("SHA-256")
            val total = file.length().coerceAtLeast(1L)
            var read = 0L
            var lastPublished = 0L
            runCatching {
                file.inputStream().use { input ->
                    val buf = ByteArray(1 shl 16)
                    while (true) {
                        coroutineContext.ensureActive()
                        val n = input.read(buf)
                        if (n < 0) break
                        md.update(buf, 0, n)
                        read += n
                        // Every ~8 MB, not every 64 KB: this is a progress bar, not a
                        // byte counter, and 3.7 GB would otherwise be ~56k emissions.
                        if (read - lastPublished >= 8L shl 20) {
                            lastPublished = read
                            setStatus(spec, OnDeviceModelStatus.Verifying(read.toFloat() / total))
                        }
                    }
                }
                md.digest().toHex() == spec.sha256.lowercase()
            }.getOrDefault(false)
        }
    }

    private fun ByteArray.toHex(): String {
        val out = StringBuilder(size * 2)
        for (b in this) {
            val v = b.toInt() and 0xff
            out.append(HEX[v ushr 4]).append(HEX[v and 0x0f])
        }
        return out.toString()
    }

    /**
     * Download [spec] to the shared models dir, resuming a prior partial file when present.
     * Updates [statuses] as it goes. Cancelling the calling coroutine pauses the
     * download (the `.part` file is kept for resume). Returns true on success.
     */
    suspend fun download(spec: OnDeviceModelSpec): Boolean {
        if (!downloading.add(spec.id)) return false
        try {
            // Before spending a multi-GB transfer: is the real file already sitting
            // there? Checked by digest rather than by name, because this directory
            // survives an uninstall — a leftover can be a truncated download, a
            // half-copied file, or a different build renamed to look right. A match
            // means we are done; a mismatch means the file is not what it claims, so it
            // is removed rather than resumed onto (a Range request appended to the wrong
            // bytes would produce a file that is the right LENGTH and still garbage).
            val target = fileFor(spec)
            if (target.exists() && spec.sha256.isNotEmpty()) {
                if (matchesDigest(spec, target)) {
                    setStatus(spec, OnDeviceModelStatus.Ready)
                    return true
                }
                target.delete()
                partFor(spec).delete()
            } else if (target.exists() && isReady(spec)) {
                // No digest to check (an import): presence is the whole test, as before.
                setStatus(spec, OnDeviceModelStatus.Ready)
                return true
            }

            return withContext(Dispatchers.IO) {
                // Retry, because a multi-GB transfer over a phone's network WILL be
                // interrupted — a dropped connection is normal, not exceptional. Each
                // attempt resumes from the .part file, so a retry costs only what was
                // actually lost. Bounded: something permanently wrong (a 404, no disk)
                // must surface rather than loop.
                var last: Throwable? = null
                for (attempt in 1..DOWNLOAD_ATTEMPTS) {
                    val before = partFor(spec).length()
                    val r = runCatching { performDownload(spec) }
                    r.onSuccess { ok -> if (ok) return@withContext true }
                    val e = r.exceptionOrNull()
                    if (e is kotlinx.coroutines.CancellationException) {
                        // A cancellation is a pause, not a failure: keep the .part file
                        // and report the partial-progress state, not an error.
                        setStatus(spec, OnDeviceModelStatus.Downloading(before, -1))
                        throw e
                    }
                    if (e == null) return@withContext false   // performDownload already reported why
                    last = e
                    // Only worth another go if the last one moved: a retry that gains
                    // nothing is a loop with extra steps.
                    if (partFor(spec).length() <= before && attempt > 1) break
                }
                setStatus(spec, OnDeviceModelStatus.Failed(describe(last)))
                false
            }
        } finally {
            downloading.remove(spec.id)
        }
    }

    private suspend fun performDownload(spec: OnDeviceModelSpec): Boolean {
        modelsDir.mkdirs()
        val partFile = partFor(spec)
        val existing = if (partFile.exists()) partFile.length() else 0L

        setStatus(spec, OnDeviceModelStatus.Downloading(existing, spec.approxBytes))

        http.newCall(requestFor(spec.downloadUrl, existing)).execute().use { resp ->
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

        // Verify BEFORE the rename, so a truncated or corrupted transfer never becomes
        // the blessed file. Without this the next launch sees a full-length file, calls
        // it Ready, and the failure surfaces as the engine refusing to load mid-chat —
        // far from the cause. The .part is dropped on a mismatch: resuming onto bytes
        // that are already wrong cannot converge.
        if (!matchesDigest(spec, partFile)) {
            partFile.delete()
            setStatus(spec, OnDeviceModelStatus.Failed("Downloaded file failed its checksum"))
            return false
        }

        if (!partFile.renameTo(fileFor(spec))) {
            setStatus(spec, OnDeviceModelStatus.Failed("Could not finalize model file"))
            return false
        }
        setStatus(spec, OnDeviceModelStatus.Ready)
        return true
    }

    /** Remove a catalog model (its partial, and its compiled cache) to reclaim storage. */
    fun delete(spec: OnDeviceModelSpec) {
        fileFor(spec).delete()
        partFor(spec).delete()
        // The cache is model-sized; leaving it would mean "delete" reclaimed half of what
        // the row said it would, with no way left in the UI to get the rest.
        cacheDirFor(spec).deleteRecursively()
        setStatus(spec, OnDeviceModelStatus.Absent)
    }

    // ---- Import (a model file the user already has on the device) -----------------------

    /**
     * Import the model at [uri] (from the system file picker) as an on-device model.
     * The file ends up OUTSIDE app-private storage: referenced in place when [uri] resolves
     * to a readable real path, otherwise copied into the shared models dir.
     *
     * A file that turns out to BE a catalog model is adopted as that model rather than
     * registered as a second, differently-named copy of it — see [adoptIfCatalog]. So the
     * returned spec is either the matched catalog entry or a fresh import; the caller only
     * needs to know it is non-null on success.
     */
    suspend fun import(uri: Uri): OnDeviceModelSpec? = withContext(Dispatchers.IO) {
        val displayName = queryDisplayName(uri)
        val id = OnDeviceModel.IMPORTED_ID_PREFIX + UUID.randomUUID().toString().take(8)

        // Prefer referencing the original file in place (no multi-GB copy).
        val realPath = resolveRealPath(uri)?.takeIf { File(it).canRead() }
        var weCopied = false
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
            weCopied = true
            dest.absolutePath to dest.length()
        }

        adoptIfCatalog(File(path), size, weCopied)?.let { return@withContext it }

        val label = displayName.substringBeforeLast('.').ifBlank { "Imported model" }
        val spec = OnDeviceModel.importedSpec(id, label, path, size)
        OnDeviceModel.registerImported(spec)
        setStatus(spec, OnDeviceModelStatus.Ready)
        _imported.update { it + spec }
        persistImports()
        spec
    }

    /**
     * Is this imported file actually one of the catalog models? If so, take it AS that
     * model and return it; otherwise null and the caller registers an ordinary import.
     *
     * Why bother: without this, importing a file you already downloaded elsewhere leaves
     * two entries for one model — the catalog's "Gemma 4 E2B", still offering to download
     * 2.6 GB, and an import called "gemma-4-E2B-it" — and the catalog's metadata (proper
     * name, RAM guidance) is lost on the copy that actually works. It is also the way an
     * ORPHANED file comes back: a model dropped from the catalog leaves its file in the
     * shared dir with no row to manage it, and re-importing that file is what makes it
     * manageable again.
     *
     * SIZE FIRST, then digest. Hashing every import would mean minutes of SHA-256 on a
     * file that was never a candidate; an exact length match against a published
     * `lfs.size` narrows it to one entry for free, and only then is a few GB worth
     * reading. It also means the progress can be reported against the entry we are
     * testing, which is what the Verifying status needs.
     */
    private suspend fun adoptIfCatalog(file: File, size: Long, weCopied: Boolean): OnDeviceModelSpec? {
        val candidate = OnDeviceModel.CATALOG.firstOrNull {
            it.sha256.isNotEmpty() && it.approxBytes == size
        } ?: return null
        if (!matchesDigest(candidate, file)) {
            // Same length, different bytes. Refresh the row we just painted Verifying on,
            // or it stays stuck at whatever fraction the hash reached.
            setStatus(candidate, if (isReady(candidate)) OnDeviceModelStatus.Ready else OnDeviceModelStatus.Absent)
            return null
        }

        val target = fileFor(candidate)
        if (file.absolutePath != target.absolutePath) {
            modelsDir.mkdirs()
            // Whatever is at the catalog path lost: we have just PROVEN these bytes are
            // the published ones, which is more than the incumbent can say.
            target.delete()
            if (!file.renameTo(target)) {
                // Cross-volume; a rename cannot span mounts, so pay for the copy. The
                // source is removed only if it was our own staging copy — a file the
                // user picked from their own storage is theirs, and import has never
                // been a move.
                val ok = runCatching { file.copyTo(target, overwrite = true) }.isSuccess
                if (!ok) return null
                if (weCopied) file.delete()
            }
        }
        setStatus(candidate, OnDeviceModelStatus.Ready)
        return candidate
    }

    /**
     * Forget an imported model. Its file is deleted only if it was COPIED into our shared
     * dir; a file referenced in place (the user's own path) is left untouched.
     */
    fun deleteImported(spec: OnDeviceModelSpec) {
        val path = spec.localPath ?: return
        val f = File(path)
        if (f.parentFile?.absolutePath == modelsDir.absolutePath) f.delete()
        // Ours either way: the cache is something we caused, not the user's file.
        cacheDirFor(spec).deleteRecursively()
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

    /**
     * Delete XNNPACK caches an older build left in the shared models dir.
     *
     * Until [cacheDirFor] existed we passed the model's own directory as `cacheDir`, so
     * LiteRT wrote `<model>.xnnpack_cache` (roughly the size of the model) right next to
     * the weights — in the folder the user browses, which is why a 2.6 GB model looked
     * like 5 GB. The new location is app-private, so those files are now unreachable
     * garbage: nothing will ever read them and no screen offers to remove them.
     *
     * Targeted, not a sweep. Only files whose name is a KNOWN model file name plus an
     * `.xnnp…` suffix are removed, so an imported model — which may be named anything —
     * cannot be caught by it. Silent: a leftover cache is not the user's problem to hear
     * about, and a failure just means it is still there next launch.
     */
    private fun sweepStrayCaches() {
        if (!hasStorageAccess()) return          // retry on a later launch once granted
        runCatching {
            val known = (OnDeviceModel.ALL.map { it.fileName } +
                _imported.value.mapNotNull { it.localPath?.substringAfterLast('/') }).toSet()
            modelsDir.listFiles()?.forEach { f ->
                if (!f.isFile) return@forEach
                val dot = f.name.indexOf(".xnnp")
                if (dot <= 0) return@forEach
                if (f.name.substring(0, dot) in known) f.delete()
            }
        }
    }

    /**
     * A message worth showing. OkHttp's transport failures often carry a terse message
     * ("stream was reset") or none at all, and a blank row tells the user nothing and us
     * less; the exception's type is the part that identifies the failure.
     */
    private fun describe(t: Throwable?): String {
        if (t == null) return "Download failed"
        val m = t.message.orEmpty()
        val kind = t::class.java.simpleName
        return if (m.isBlank()) kind else "$kind: $m"
    }

    private companion object {
        const val KEY_IMPORTS = "imports"
        /** Enough to ride out a handful of dropped connections, few enough to end. */
        const val DOWNLOAD_ATTEMPTS = 4
        const val USER_AGENT = "Mirobody-Android/1.0 (+https://thetahealth.ai)"
    }
}

private val HEX = "0123456789abcdef".toCharArray()

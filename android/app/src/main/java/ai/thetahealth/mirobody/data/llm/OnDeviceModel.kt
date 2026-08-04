package ai.thetahealth.mirobody.data.llm

/**
 * One on-device model the app can run. LiteRT-LM `.litertlm` files, edge-tuned and
 * published under the Apache-2.0 `litert-community` HF org (ungated, no token needed).
 * Each file is downloaded at runtime (never bundled in the APK) into app-private
 * storage; several can coexist and the user picks which to run.
 *
 * [providerCode] is the synthetic chat-provider id this model surfaces as in the
 * picker (see [ai.thetahealth.mirobody.data.chat.dto.ProviderInfo]).
 */
data class OnDeviceModelSpec(
    /** Stable id, used for the provider code and the on-disk filename mapping. */
    val id: String,
    /** Non-localized display name, e.g. "Gemma 4 E2B" (kept stable across UI languages). */
    val displayName: String,
    /** Filename under the shared models dir (download target / basename of an import). */
    val fileName: String,
    /** Direct Hugging Face download URL (`resolve/main/<file>?download=true`); "" when imported. */
    val downloadUrl: String,
    /** Approximate download size, for the UI to show before the Content-Length lands. */
    val approxBytes: Long,
    /** Recommended device RAM (peak inference footprint is device/context-dependent). */
    val recommendedRam: String,
    /**
     * Absolute path to a user-imported model file that lives OUTSIDE the app's control
     * (picked from the filesystem), or null for a catalog model that lives under the
     * shared models dir. When set, the model is loaded from here as-is — never downloaded,
     * and never deleted from disk on removal (it's the user's own file).
     */
    val localPath: String? = null,
) {
    val providerCode: String get() = OnDeviceModel.PROVIDER_PREFIX + id

    /** True for a user-imported model (referenced by [localPath]), false for a catalog model. */
    val isImported: Boolean get() = localPath != null
}

/**
 * The catalog of on-device models the app ships support for. Desktop (llama.cpp) is
 * model-agnostic over any GGUF; mobile (LiteRT-LM) ships a curated set of `.litertlm`
 * models, since the runtime only accepts that format.
 */
object OnDeviceModel {
    /** Prefix marking a synthetic on-device provider code (never collides with a server model). */
    const val PROVIDER_PREFIX: String = "__ondevice__/"

    /** Id prefix for a user-imported model, so it never collides with a catalog id. */
    const val IMPORTED_ID_PREFIX: String = "imported-"

    /**
     * User-imported models, registered at runtime by [ModelManager] from its persisted
     * list. Kept alongside [CATALOG] so [byId] / [byProviderCode] — and therefore
     * `ProviderInfo.modelSpec` and the whole chat/engine path — resolve imports with no
     * further changes. Keyed by [OnDeviceModelSpec.id].
     */
    private val imported = java.util.concurrent.ConcurrentHashMap<String, OnDeviceModelSpec>()

    /** Register (or replace) an imported model so it resolves through [byId]/[byProviderCode]. */
    fun registerImported(spec: OnDeviceModelSpec) { imported[spec.id] = spec }

    /** Forget an imported model (does not touch the file on disk). */
    fun unregisterImported(id: String) { imported.remove(id) }

    /** Currently-registered imported models, name-sorted for stable UI ordering. */
    fun importedSpecs(): List<OnDeviceModelSpec> = imported.values.sortedBy { it.displayName }

    /** Build an imported spec from a picked file's absolute [path] and known [sizeBytes]. */
    fun importedSpec(id: String, displayName: String, path: String, sizeBytes: Long): OnDeviceModelSpec =
        OnDeviceModelSpec(
            id = id,
            displayName = displayName,
            fileName = path.substringAfterLast('/'),
            downloadUrl = "",
            approxBytes = sizeBytes,
            recommendedRam = "—",
            localPath = path,
        )

    // Ordered smallest → largest so low-memory devices see the light options first.
    // `approxBytes` is only the pre-flight estimate; the real size comes from the
    // download's Content-Length. Bigger models want more RAM at inference (~model size
    // plus overhead), so a 4B model is best on an 8 GB+ phone.
    val CATALOG: List<OnDeviceModelSpec> = listOf(
        OnDeviceModelSpec(
            id = "qwen3-0.6b",
            displayName = "Qwen3 0.6B (int4)",
            fileName = "qwen3-0.6b-int4.litertlm",
            downloadUrl = "https://huggingface.co/litert-community/Qwen3-0.6B/resolve/main/qwen3_0_6b_mixed_int4.litertlm?download=true",
            approxBytes = 497_664_000L,
            recommendedRam = "4 GB+",
        ),
        OnDeviceModelSpec(
            id = "qwen2.5-1.5b",
            displayName = "Qwen2.5 1.5B",
            fileName = "qwen2.5-1.5b-instruct-q8.litertlm",
            downloadUrl = "https://huggingface.co/litert-community/Qwen2.5-1.5B-Instruct/resolve/main/Qwen2.5-1.5B-Instruct_multi-prefill-seq_q8_ekv4096.litertlm?download=true",
            approxBytes = 1_597_931_520L,
            recommendedRam = "6 GB+",
        ),
        OnDeviceModelSpec(
            id = "qwen3-1.7b",
            displayName = "Qwen3 1.7B",
            fileName = "qwen3-1.7b.litertlm",
            downloadUrl = "https://huggingface.co/litert-community/Qwen3-1.7B/resolve/main/Qwen3_1.7B.litertlm?download=true",
            approxBytes = 2_056_729_520L,
            recommendedRam = "6 GB+",
        ),
        OnDeviceModelSpec(
            id = "gemma-4-e2b",
            displayName = "Gemma 4 E2B",
            fileName = "gemma-4-E2B-it.litertlm",
            downloadUrl = "https://huggingface.co/litert-community/gemma-4-E2B-it-litert-lm/resolve/main/gemma-4-E2B-it.litertlm?download=true",
            approxBytes = 2_588_147_712L,
            recommendedRam = "6 GB+",
        ),
        OnDeviceModelSpec(
            id = "qwen3-4b-instruct",
            displayName = "Qwen3 4B (Instruct)",
            fileName = "qwen3-4b-instruct.litertlm",
            downloadUrl = "https://huggingface.co/litert-community/Qwen3-4B-Instruct-2507/resolve/main/qwen3_4b_instruct_2507_mixed_int4.litertlm?download=true",
            approxBytes = 2_659_057_664L,
            recommendedRam = "8 GB+",
        ),
        OnDeviceModelSpec(
            id = "gemma-4-e4b",
            displayName = "Gemma 4 E4B",
            fileName = "gemma-4-E4B-it.litertlm",
            downloadUrl = "https://huggingface.co/litert-community/gemma-4-E4B-it-litert-lm/resolve/main/gemma-4-E4B-it.litertlm?download=true",
            approxBytes = 3_659_530_240L,
            recommendedRam = "8 GB+",
        ),
    )

    fun byId(id: String): OnDeviceModelSpec? =
        CATALOG.firstOrNull { it.id == id } ?: imported[id]

    fun byProviderCode(code: String): OnDeviceModelSpec? =
        CATALOG.firstOrNull { it.providerCode == code }
            ?: imported.values.firstOrNull { it.providerCode == code }
}

/** Lifecycle of an on-device model file on this device. */
sealed interface OnDeviceModelStatus {
    /** Not downloaded yet. */
    data object Absent : OnDeviceModelStatus

    /** Download in progress. [totalBytes] is -1 until the server reports Content-Length. */
    data class Downloading(val downloadedBytes: Long, val totalBytes: Long) : OnDeviceModelStatus {
        val fraction: Float
            get() = if (totalBytes > 0) (downloadedBytes.toFloat() / totalBytes).coerceIn(0f, 1f) else 0f
    }

    /** Present on disk and ready to load. */
    data object Ready : OnDeviceModelStatus

    /** Last download attempt failed; [message] is user-facing. */
    data class Failed(val message: String) : OnDeviceModelStatus
}

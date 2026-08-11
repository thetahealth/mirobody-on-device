package ai.thetahealth.mirobody.data.llm

/**
 * The accelerator a model file was built for. Our own enum rather than LiteRT's `Backend`
 * so the catalog stays a plain data description — the engine maps it at the boundary.
 *
 * Only the two we can actually ship: `Backend.NPU` needs a vendor library directory and a
 * per-SoC file, and `Backend.GOOGLE_TENSOR` needs a Pixel; neither has a build for every
 * catalog model. Add a case when there is a file to point it at.
 */
enum class OnDeviceBackend { CPU, GPU }

/**
 * Which engine runs this file. The format follows from it: LiteRT-LM takes `.litertlm`,
 * llama.cpp takes `.gguf`, and neither reads the other's.
 *
 * Two runtimes on purpose. LiteRT-LM is a Gradle dependency with vendor GPU/NPU builds;
 * llama.cpp is cross-compiled into libmirobody.so (android/build-llama.cmd) and reads any
 * GGUF ever published. They answer different questions — see docs/on-device-llm.md — and
 * a model says which one it needs rather than the app guessing from the extension.
 */
enum class OnDeviceRuntime { LITERT_LM, LLAMA_CPP }

/**
 * One on-device model the app can run — a `.litertlm` for LiteRT-LM or a `.gguf` for
 * llama.cpp, per [runtime]. Each file is downloaded at runtime (never bundled in the
 * APK); several can coexist and the user picks which to run.
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
    /**
     * Download size. For a CATALOG entry this is the file's **exact** length (Hugging
     * Face's `lfs.size`), which is what makes it usable as a cheap presence gate as well
     * as a pre-flight estimate; for an import it is the picked file's real size.
     */
    val approxBytes: Long,
    /** Recommended device RAM (peak inference footprint is device/context-dependent). */
    val recommendedRam: String,
    /**
     * Lowercase hex SHA-256 of the model file — Hugging Face's LFS `oid`, which is the
     * only digest it publishes (there is no MD5 to be had without downloading all four
     * files and hashing them ourselves, and SHA-256 answers the same question).
     *
     * Empty for an imported model: the user's own file has no authoritative digest to
     * check against, and its `id` is already unique per import.
     *
     * This is what lets "already on disk" mean the RIGHT file. Model files live in
     * shared storage that survives an uninstall, so a leftover can be a truncated
     * download, a half-copied file, or a different build renamed to look right.
     */
    val sha256: String = "",
    /**
     * Which accelerator this particular FILE was built for.
     *
     * Not a runtime switch: `litert-community` publishes a separate build per backend
     * (`gemma-4-E2B-it.litertlm` vs `gemma-4-E2B-it-gpu.litertlm`, and per-SoC files for
     * NPU), so the backend travels with the download rather than beside it. Handing the
     * CPU file to `Backend.GPU()` is not a configuration, it is a mismatch.
     */
    val backend: OnDeviceBackend = OnDeviceBackend.CPU,
    /** Which engine loads this file; the default is the one the catalog has always used. */
    val runtime: OnDeviceRuntime = OnDeviceRuntime.LITERT_LM,
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
    // Bigger models want more RAM at inference (~model size plus overhead), so a 4B
    // model is best on an 8 GB+ phone.
    //
    // Every `approxBytes` / `sha256` pair below is Hugging Face's own `lfs.size` /
    // `lfs.oid` for that exact file, read from the repo's tree API — not estimated, and
    // not computed here. Re-read them from the API if a URL is ever repointed: a stale
    // digest turns every "already downloaded" check into a re-download.
    val CATALOG: List<OnDeviceModelSpec> = listOf(
        // Two lanes on purpose, and the Qwen entries are why. As of 2026-08-09
        // `litert-community` still publishes nothing above Qwen3.5-0.8B, so the current
        // Qwen generation reaches a phone only as GGUF — which is exactly what the
        // second runtime is for. Gemma stays on `.litertlm`, where Google's own runtime
        // reads the E-series' MatFormer layout properly and beats llama.cpp by 1.5x.
        // Swap a Qwen entry over the day a .litertlm lands and it measures faster.
        OnDeviceModelSpec(
            id = "qwen35-2b",
            displayName = "Qwen3.5 2B",
            fileName = "Qwen3.5-2B-Q4_K_M.gguf",
            downloadUrl = "https://huggingface.co/unsloth/Qwen3.5-2B-GGUF/resolve/main/Qwen3.5-2B-Q4_K_M.gguf?download=true",
            approxBytes = 1_280_835_840L,
            sha256 = "aaf42c8b7c3cab2bf3d69c355048d4a0ee9973d48f16c731c0520ee914699223",
            recommendedRam = "4 GB+",
            runtime = OnDeviceRuntime.LLAMA_CPP,
        ),
        OnDeviceModelSpec(
            id = "gemma-4-e2b",
            displayName = "Gemma 4 E2B",
            fileName = "gemma-4-E2B-it.litertlm",
            downloadUrl = "https://huggingface.co/litert-community/gemma-4-E2B-it-litert-lm/resolve/main/gemma-4-E2B-it.litertlm?download=true",
            approxBytes = 2_588_147_712L,
            sha256 = "181938105e0eefd105961417e8da75903eacda102c4fce9ce90f50b97139a63c",
            recommendedRam = "6 GB+",
        ),
        OnDeviceModelSpec(
            id = "qwen35-4b",
            displayName = "Qwen3.5 4B",
            fileName = "Qwen3.5-4B-Q4_K_M.gguf",
            downloadUrl = "https://huggingface.co/unsloth/Qwen3.5-4B-GGUF/resolve/main/Qwen3.5-4B-Q4_K_M.gguf?download=true",
            approxBytes = 2_740_937_888L,
            sha256 = "00fe7986ff5f6b463e62455821146049db6f9313603938a70800d1fb69ef11a4",
            recommendedRam = "8 GB+",
            runtime = OnDeviceRuntime.LLAMA_CPP,
        ),
        OnDeviceModelSpec(
            id = "gemma-4-e4b",
            displayName = "Gemma 4 E4B",
            fileName = "gemma-4-E4B-it.litertlm",
            downloadUrl = "https://huggingface.co/litert-community/gemma-4-E4B-it-litert-lm/resolve/main/gemma-4-E4B-it.litertlm?download=true",
            approxBytes = 3_659_530_240L,
            sha256 = "0b2a8980ce155fd97673d8e820b4d29d9c7d99b8fa6806f425d969b145bd52e0",
            recommendedRam = "8 GB+",
        ),
    )

    /**
     * Builds that exist to be MEASURED, not offered.
     *
     * The GPU twin of a catalog model is the same weights compiled for a different
     * accelerator: same answers, different speed and memory. Putting it in [CATALOG]
     * would ask every user to have an opinion about a build target in order to pick a
     * model, so it lives here and surfaces only in the debug probe, where comparing two
     * backends on one device is the entire point.
     *
     * Keep it OUT of the picker for the same reason: two entries named "Gemma 4 E2B"
     * differing by an acronym is a worse chooser than one.
     */
    val DEBUG_MODELS: List<OnDeviceModelSpec> = listOf(
        // --- GGUF, run by llama.cpp -------------------------------------------------
        // Here rather than in CATALOG because the chat path cannot use them yet: the
        // streaming JNI engine is not written, only the benchmark. Offering a model the
        // picker cannot actually talk to would be worse than not offering it.
        //
        // Q4_K_M against LiteRT's int8-ish builds is the comparison worth having: decode
        // is bound by bytes read per token, so the quantization IS the variable, and
        // these are the same two models already measured on the other runtime.
        OnDeviceModelSpec(
            id = "gemma-4-e2b-gguf",
            displayName = "Gemma 4 E2B (GGUF Q4_K_M)",
            fileName = "gemma-4-E2B-it-Q4_K_M.gguf",
            downloadUrl = "https://huggingface.co/unsloth/gemma-4-E2B-it-GGUF/resolve/main/gemma-4-E2B-it-Q4_K_M.gguf?download=true",
            approxBytes = 3_106_738_272L,
            sha256 = "740185b21d22ceb83a11c3aa62ad5842ef32c70f6096d756bbee85a1e4ec34b8",
            recommendedRam = "6 GB+",
            runtime = OnDeviceRuntime.LLAMA_CPP,
        ),
        OnDeviceModelSpec(
            id = "qwen3-4b-gguf",
            displayName = "Qwen3 4B Instruct (GGUF Q4_K_M)",
            fileName = "Qwen3-4B-Instruct-2507-Q4_K_M.gguf",
            downloadUrl = "https://huggingface.co/unsloth/Qwen3-4B-Instruct-2507-GGUF/resolve/main/Qwen3-4B-Instruct-2507-Q4_K_M.gguf?download=true",
            approxBytes = 2_497_281_120L,
            sha256 = "3605803b982cb64aead44f6c1b2ae36e3acdb41d8e46c8a94c6533bc4c67e597",
            recommendedRam = "8 GB+",
            runtime = OnDeviceRuntime.LLAMA_CPP,
        ),
        // --- .litertlm builds the catalog no longer offers ---------------------------
        // Qwen on LiteRT-LM, which the catalog dropped when Qwen3.5 arrived GGUF-only.
        // Kept because they are one half of the measurement in docs/on-device-llm.md —
        // "which engine is faster" is only answerable while both files are downloadable
        // — and because a device that already has one should not find it unrecognized.
        OnDeviceModelSpec(
            id = "qwen3-1.7b",
            displayName = "Qwen3 1.7B (LiteRT)",
            fileName = "qwen3-1.7b.litertlm",
            downloadUrl = "https://huggingface.co/litert-community/Qwen3-1.7B/resolve/main/Qwen3_1.7B.litertlm?download=true",
            approxBytes = 2_056_729_520L,
            sha256 = "66064a4e9269cb693e124c4e3040bcb8a446b10bca42663896329495add3861c",
            recommendedRam = "6 GB+",
        ),
        OnDeviceModelSpec(
            id = "qwen3-4b-instruct",
            displayName = "Qwen3 4B Instruct (LiteRT)",
            fileName = "qwen3-4b-instruct.litertlm",
            downloadUrl = "https://huggingface.co/litert-community/Qwen3-4B-Instruct-2507/resolve/main/qwen3_4b_instruct_2507_mixed_int4.litertlm?download=true",
            approxBytes = 2_659_057_664L,
            sha256 = "9e48b165836256f5344d9d044930607b9c47f6ef34e27f82e96881664f3ba2fd",
            recommendedRam = "8 GB+",
        ),

        // --- the GPU twin of a .litertlm model --------------------------------------
        OnDeviceModelSpec(
            id = "gemma-4-e2b-gpu",
            displayName = "Gemma 4 E2B (GPU)",
            fileName = "gemma-4-E2B-it-gpu.litertlm",
            downloadUrl = "https://huggingface.co/litert-community/gemma-4-E2B-it-litert-lm/resolve/main/gemma-4-E2B-it-gpu.litertlm?download=true",
            approxBytes = 2_008_432_640L,
            sha256 = "a53a59001894c58e6bdb5b9b227709f91a2e3e556baa7d85acf9c55402ba5cf5",
            recommendedRam = "6 GB+",
            backend = OnDeviceBackend.GPU,
        ),
    )

    /** Everything we know how to download — the offered list plus the measured ones. */
    val ALL: List<OnDeviceModelSpec> get() = CATALOG + DEBUG_MODELS

    fun byId(id: String): OnDeviceModelSpec? =
        ALL.firstOrNull { it.id == id } ?: imported[id]

    fun byProviderCode(code: String): OnDeviceModelSpec? =
        ALL.firstOrNull { it.providerCode == code }
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

    /**
     * Hashing a file already on disk to decide whether it is the real thing. Its own
     * state because it is not instant — a few GB of SHA-256 takes seconds, and a
     * progress-less pause would read as a hang.
     */
    data class Verifying(val fraction: Float) : OnDeviceModelStatus

    /** Present on disk and ready to load. */
    data object Ready : OnDeviceModelStatus

    /** Last download attempt failed; [message] is user-facing. */
    data class Failed(val message: String) : OnDeviceModelStatus
}

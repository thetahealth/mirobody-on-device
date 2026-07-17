package ai.thetahealth.mirobody.data.llm

/**
 * The on-device model we ship support for. Gemma 4 E2B in LiteRT-LM format — the
 * edge-tuned variant Google publishes under the Apache-2.0 `litert-community` org
 * (ungated, no HF token needed).
 *
 * The `.litertlm` file is ~2.5 GB and needs ~3 GB of RAM at inference time, so it is
 * downloaded at runtime (never bundled in the APK) and stored in app-private storage.
 */
object OnDeviceModel {
    const val DISPLAY_NAME: String = "Gemma 4 E2B (instruction-tuned)"

    /** Local filename under <filesDir>/models/. */
    const val FILE_NAME: String = "gemma-4-E2B-it.litertlm"

    /**
     * Direct download URL on Hugging Face. `resolve/main/<file>?download=true` streams
     * the raw LFS blob. The repo is Apache-2.0 and ungated; if Google later gates it,
     * a token header would be needed here (see [ModelManager]).
     */
    const val DOWNLOAD_URL: String =
        "https://huggingface.co/litert-community/gemma-4-E2B-it-litert-lm/resolve/main/" + FILE_NAME + "?download=true"

    /** Approximate download size, for the UI to show before the Content-Length lands. */
    const val APPROX_BYTES: Long = 2_583L * 1024 * 1024
}

/** Lifecycle of the on-device model file on this device. */
sealed interface OnDeviceModelStatus {
    /** Not downloaded yet. */
    data object Absent : OnDeviceModelStatus

    /** Download in progress. [total] is -1 until the server reports Content-Length. */
    data class Downloading(val downloadedBytes: Long, val totalBytes: Long) : OnDeviceModelStatus {
        val fraction: Float
            get() = if (totalBytes > 0) (downloadedBytes.toFloat() / totalBytes).coerceIn(0f, 1f) else 0f
    }

    /** Present on disk and ready to load. */
    data object Ready : OnDeviceModelStatus

    /** Last download attempt failed; [message] is user-facing. */
    data class Failed(val message: String) : OnDeviceModelStatus
}

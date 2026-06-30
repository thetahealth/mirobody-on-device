package ai.thetahealth.mirobody.data.chat.dto

import kotlinx.serialization.Serializable

@Serializable
data class ProviderInfo(
    val name: String = "",
    val code: String = "",
) {
    val agentCode: String get() = name.substringBefore('/', missingDelimiterValue = "")

    /**
     * True for the synthetic, client-only "on-device" provider. When this is the
     * selected provider the chat layer routes to [ai.thetahealth.mirobody.data.llm.OnDeviceLlmEngine]
     * instead of opening the `/api/chat` SSE stream — fully offline, no server.
     */
    val isOnDevice: Boolean get() = code == ON_DEVICE_CODE

    companion object {
        /** Sentinel `code` marking the on-device provider (never collides with a server code). */
        const val ON_DEVICE_CODE: String = "__ondevice_gemma4__"

        /**
         * Stable display name. Kept non-localized so the persisted "selected provider"
         * preference (keyed by name) survives a UI-language change.
         */
        const val ON_DEVICE_NAME: String = "Gemma 4 · On-device"

        /** The synthetic provider injected into the picker on every client. */
        val onDevice: ProviderInfo = ProviderInfo(name = ON_DEVICE_NAME, code = ON_DEVICE_CODE)
    }
}

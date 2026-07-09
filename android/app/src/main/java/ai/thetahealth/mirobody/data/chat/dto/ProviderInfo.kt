package ai.thetahealth.mirobody.data.chat.dto

import kotlinx.serialization.Serializable

/**
 * One agent group from `GET /api/providers`, which returns `[{ agent, providers[] }]`.
 * The default agent's group carries an empty [agent], so its models list bare and
 * route through the default agent server-side.
 */
@Serializable
data class ProviderGroup(
    val agent: String = "",
    val providers: List<String> = emptyList(),
)

/**
 * A single selectable provider, flattened from a [ProviderGroup]: the [agent]
 * (empty for the default agent) plus one [provider] (model). Submitted verbatim as
 * the chat request's `agent` / `provider` — no string parsing.
 */
data class ProviderInfo(
    val agent: String = "",
    val provider: String = "",
) {
    /** Picker display: the model name (or the on-device label). */
    val label: String get() = if (isOnDevice) ON_DEVICE_NAME else provider

    /** Stable key for persisting / restoring the selection. */
    val key: String get() = if (agent.isEmpty()) provider else "$agent/$provider"

    /**
     * True for the synthetic, client-only "on-device" provider. When this is the
     * selected provider the chat layer routes to [ai.thetahealth.mirobody.data.llm.OnDeviceLlmEngine]
     * instead of opening the `/api/chat` SSE stream — fully offline, no server.
     */
    val isOnDevice: Boolean get() = provider == ON_DEVICE_CODE

    companion object {
        /** Sentinel `provider` marking the on-device provider (never collides with a server model). */
        const val ON_DEVICE_CODE: String = "__ondevice_gemma4__"

        /**
         * Stable display name. Kept non-localized so the persisted "selected provider"
         * preference (keyed by [key]) survives a UI-language change.
         */
        const val ON_DEVICE_NAME: String = "Gemma 4 · On-device"

        /** The synthetic provider injected into the picker on every client. */
        val onDevice: ProviderInfo = ProviderInfo(agent = "", provider = ON_DEVICE_CODE)
    }
}

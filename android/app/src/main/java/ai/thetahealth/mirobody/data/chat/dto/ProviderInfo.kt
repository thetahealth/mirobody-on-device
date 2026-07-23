package ai.thetahealth.mirobody.data.chat.dto

import ai.thetahealth.mirobody.data.llm.OnDeviceModel
import ai.thetahealth.mirobody.data.llm.OnDeviceModelSpec
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
 *
 * Two synthetic, client-only kinds also flow through here (mirroring the Qt desktop
 * client): a **manage** entry that opens the on-device model manager, and one entry
 * per **downloaded on-device model**. Both route to the local
 * [ai.thetahealth.mirobody.data.llm.OnDeviceLlmEngine] rather than `/api/chat`.
 */
data class ProviderInfo(
    val agent: String = "",
    val provider: String = "",
) {
    /** Picker display text. */
    val label: String get() = when {
        isManageEntry -> ON_DEVICE_MANAGE_NAME
        else -> modelSpec?.let { it.displayName + ON_DEVICE_SUFFIX } ?: provider
    }

    /** Stable key for persisting / restoring the selection. */
    val key: String get() = if (agent.isEmpty()) provider else "$agent/$provider"

    /**
     * The "Manage on-device AI" entry. Selecting it opens the model manager instead of
     * starting a chat turn.
     */
    val isManageEntry: Boolean get() = provider == ON_DEVICE_MANAGE_CODE

    /** The concrete on-device model this entry runs, or null (server model / manage entry). */
    val modelSpec: OnDeviceModelSpec? get() = OnDeviceModel.byProviderCode(provider)

    /**
     * True for any client-only on-device entry (the manage entry or a downloaded model).
     * When the selected provider is one of these the chat layer routes to the on-device
     * engine instead of opening the `/api/chat` SSE stream — fully offline, no server.
     */
    val isOnDevice: Boolean get() = isManageEntry || modelSpec != null

    companion object {
        /** Sentinel `provider` for the manage entry (never collides with a server model). */
        const val ON_DEVICE_MANAGE_CODE: String = "__ondevice_manage__"

        /**
         * Non-localized labels, so the persisted "selected provider" preference (keyed by
         * [key]) survives a UI-language change. Model names come from the catalog.
         */
        const val ON_DEVICE_MANAGE_NAME: String = "On-device AI…"
        const val ON_DEVICE_SUFFIX: String = " · On-device"

        /** The manage entry, injected into the picker on every client. */
        val manage: ProviderInfo = ProviderInfo(agent = "", provider = ON_DEVICE_MANAGE_CODE)

        /** A picker entry for a downloaded on-device model. */
        fun forModel(spec: OnDeviceModelSpec): ProviderInfo =
            ProviderInfo(agent = "", provider = spec.providerCode)
    }
}

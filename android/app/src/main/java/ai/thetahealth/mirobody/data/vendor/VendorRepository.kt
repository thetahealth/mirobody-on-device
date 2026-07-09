package ai.thetahealth.mirobody.data.vendor

import ai.thetahealth.mirobody.data.net.ensureOk
import ai.thetahealth.mirobody.data.net.unwrap

class VendorRepository(private val api: VendorApi) {

    /** The user's connected vendors, keyed by id. */
    suspend fun connected(): Map<String, VendorLink> =
        api.list().unwrap().associateBy { it.id }

    /** Server-resolved brand icons { id -> data URI }. Best-effort (empty on error). */
    suspend fun icons(): Map<String, String> =
        runCatching { api.icons().unwrap() }.getOrDefault(emptyMap())

    /** The vendor's OAuth consent URL to open in a browser, or null. */
    suspend fun authorizeUrl(id: String): String? =
        api.authorize(id).unwrap().authorizeUrl

    /** Revoke the grant + delete stored tokens for a vendor. */
    suspend fun unlink(id: String) {
        api.unlink(id).ensureOk()
    }
}

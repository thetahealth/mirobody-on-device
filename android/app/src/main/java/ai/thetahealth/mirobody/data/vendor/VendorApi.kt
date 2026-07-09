package ai.thetahealth.mirobody.data.vendor

import ai.thetahealth.mirobody.data.net.ApiEnvelope
import kotlinx.serialization.SerialName
import kotlinx.serialization.Serializable
import kotlinx.serialization.json.JsonElement
import retrofit2.http.GET
import retrofit2.http.POST
import retrofit2.http.Path

/** A connected vendor grant from GET /vendors (only the fields the UI needs;
 *  unknown keys like has_token / updated_at are ignored). */
@Serializable
data class VendorLink(val id: String, val verified: Boolean = false)

/** GET /vendors/{id}/authorize -> the vendor's OAuth consent URL. */
@Serializable
data class VendorAuthorize(@SerialName("authorize_url") val authorizeUrl: String? = null)

/**
 * Vendor account management (the /vendors endpoints), the Android counterpart of the web
 * client's vendors.js. Bearer auth is added by AuthInterceptor. Icons are public
 * (server-resolved, same-origin) so they carry no third-party call. Connect is a
 * browser hand-off: GET authorize -> open the URL; the vendor redirects to the
 * server callback, which stores the grant, so the app just re-lists on return.
 */
interface VendorApi {
    @GET("/vendors")
    suspend fun list(): ApiEnvelope<List<VendorLink>>

    @GET("/vendors/icons")
    suspend fun icons(): ApiEnvelope<Map<String, String>>

    @GET("/vendors/{id}/authorize")
    suspend fun authorize(@Path("id") id: String): ApiEnvelope<VendorAuthorize>

    @POST("/vendors/{id}/unlink")
    suspend fun unlink(@Path("id") id: String): ApiEnvelope<JsonElement?>
}

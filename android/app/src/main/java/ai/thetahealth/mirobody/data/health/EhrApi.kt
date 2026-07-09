package ai.thetahealth.mirobody.data.health

import ai.thetahealth.mirobody.data.net.ApiEnvelope
import kotlinx.serialization.SerialName
import kotlinx.serialization.Serializable
import retrofit2.http.Body
import retrofit2.http.GET
import retrofit2.http.POST
import retrofit2.http.Query

/** One tenant from the EHR provider directory. */
@Serializable
data class EhrProvider(
    val name: String = "",
    @SerialName("fhir_base_url") val fhirBaseUrl: String,
)

@Serializable
data class EhrProviders(val providers: List<EhrProvider> = emptyList())

@Serializable
data class EhrAuthorizeRequest(@SerialName("fhir_base_url") val fhirBaseUrl: String)

@Serializable
data class EhrAuthorize(@SerialName("authorize_url") val authorizeUrl: String? = null)

@Serializable
data class EhrSyncResult(val posted: Int = 0)

/**
 * EHR connect (SMART on FHIR), the Android counterpart of the web client's ehr.js.
 * Connect is a browser hand-off: POST authorize -> open the URL; the EHR redirects
 * to the server callback, which exchanges the code and stores the link. The app
 * then pulls records with sync. Bearer auth is added by AuthInterceptor.
 */
interface EhrApi {
    @GET("/health/ehr/providers")
    suspend fun providers(@Query("q") query: String): ApiEnvelope<EhrProviders>

    @POST("/health/ehr/authorize")
    suspend fun authorize(@Body body: EhrAuthorizeRequest): ApiEnvelope<EhrAuthorize>

    @POST("/health/ehr/sync")
    suspend fun sync(): ApiEnvelope<EhrSyncResult>
}

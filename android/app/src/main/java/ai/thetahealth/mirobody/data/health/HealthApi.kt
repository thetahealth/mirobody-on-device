package ai.thetahealth.mirobody.data.health

import kotlinx.serialization.json.JsonElement
import kotlinx.serialization.json.JsonObject
import retrofit2.Response
import retrofit2.http.Body
import retrofit2.http.POST

/**
 * The FHIR ingestion endpoint on the embedded server. Unlike the other APIs in
 * this app it does NOT use the `{code,msg,data}` ApiEnvelope: /fhir speaks FHIR,
 * returning the created resource (201) or an OperationOutcome (4xx). The bearer
 * token is attached automatically by AuthInterceptor, and the base URL is the
 * loopback embedded server, so this needs no extra wiring.
 */
interface HealthApi {
    /** Create one Observation. Returns the raw FHIR response for status inspection. */
    @POST("/fhir/Observation")
    suspend fun postObservation(@Body observation: JsonObject): Response<JsonElement>
}

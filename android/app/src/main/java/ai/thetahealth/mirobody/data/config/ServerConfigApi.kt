package ai.thetahealth.mirobody.data.config

import retrofit2.http.GET

/**
 * The `/mirobody.json` endpoint is a public, unwrapped JSON document — it does NOT
 * use the `{code, message, data}` envelope that the rest of the API uses.
 */
interface ServerConfigApi {
    @GET("/mirobody.json")
    suspend fun fetch(): ServerConfig
}
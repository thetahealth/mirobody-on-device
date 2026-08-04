package ai.thetahealth.mirobody.data.config

import ai.thetahealth.mirobody.data.net.ApiEnvelope
import retrofit2.http.GET

/**
 * Sign-in capability discovery. Public and unauthenticated -- the sign-in screen
 * needs it before there are any credentials to send.
 *
 * Unlike the `/mirobody.json` document this replaced, it uses the project's
 * `{code, message, data}` envelope like the rest of the API. The envelope code is
 * always 0 (no provider configured is a valid answer, not an error), so callers
 * read the per-provider flags rather than the code.
 */
interface ServerConfigApi {
    @GET("/auth/providers")
    suspend fun fetch(): ApiEnvelope<ServerConfig>
}

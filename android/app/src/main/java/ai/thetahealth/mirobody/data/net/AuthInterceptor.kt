package ai.thetahealth.mirobody.data.net

import kotlinx.coroutines.flow.StateFlow
import okhttp3.Interceptor
import okhttp3.Response

/** Injects `Authorization: Bearer <token>` when a token is present. */
class AuthInterceptor(
    private val tokenFlow: StateFlow<String?>,
) : Interceptor {

    override fun intercept(chain: Interceptor.Chain): Response {
        val token = tokenFlow.value
        val req = chain.request()
        val withAuth = if (!token.isNullOrBlank() && req.header("Authorization") == null) {
            req.newBuilder().header("Authorization", "Bearer $token").build()
        } else {
            req
        }
        return chain.proceed(withAuth)
    }
}
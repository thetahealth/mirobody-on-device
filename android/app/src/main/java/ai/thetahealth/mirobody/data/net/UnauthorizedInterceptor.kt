package ai.thetahealth.mirobody.data.net

import ai.thetahealth.mirobody.data.settings.SettingsStore
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.launch
import okhttp3.Interceptor
import okhttp3.Response

/** On 401, clear the persisted token so the UI (watching tokenFlow) routes back to login. */
class UnauthorizedInterceptor(
    private val settings: SettingsStore,
    private val appScope: CoroutineScope,
) : Interceptor {

    override fun intercept(chain: Interceptor.Chain): Response {
        val response = chain.proceed(chain.request())
        if (response.code == 401) {
            appScope.launch { settings.clearAuth() }
        }
        return response
    }
}
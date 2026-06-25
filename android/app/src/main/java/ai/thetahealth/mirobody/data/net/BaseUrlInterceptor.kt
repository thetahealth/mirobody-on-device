package ai.thetahealth.mirobody.data.net

import ai.thetahealth.mirobody.data.settings.SettingsStore
import kotlinx.coroutines.flow.StateFlow
import okhttp3.HttpUrl.Companion.toHttpUrlOrNull
import okhttp3.Interceptor
import okhttp3.Response
import java.io.IOException

/**
 * Rewrites every request URL to use the configured base (scheme + host + port).
 * Retrofit is constructed with a placeholder base; this interceptor is the real source of truth.
 * When no base URL is configured, falls back to the self-hosted [SettingsStore.DEFAULT_BASE_URL].
 */
class BaseUrlInterceptor(
    private val baseUrlFlow: StateFlow<String?>,
) : Interceptor {

    override fun intercept(chain: Interceptor.Chain): Response {
        val raw = baseUrlFlow.value?.takeIf(String::isNotBlank) ?: SettingsStore.DEFAULT_BASE_URL
        val base = raw.toHttpUrlOrNull()
            ?: throw IOException("Base URL is invalid: $raw")

        val req = chain.request()
        val newUrl = req.url.newBuilder()
            .scheme(base.scheme)
            .host(base.host)
            .port(base.port)
            .build()
        return chain.proceed(req.newBuilder().url(newUrl).build())
    }
}
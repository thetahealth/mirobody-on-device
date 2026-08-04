package ai.thetahealth.mirobody.data.config

import ai.thetahealth.mirobody.data.net.unwrap
import ai.thetahealth.mirobody.data.settings.SettingsStore
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.flow.asStateFlow
import kotlinx.coroutines.flow.distinctUntilChanged
import kotlinx.coroutines.flow.first
import kotlinx.coroutines.launch
import kotlinx.serialization.json.Json

/**
 * Fetches and caches the server's sign-in capability document (GET /auth/providers)
 * for the current base URL.
 *
 * Behavior:
 *  - On construction, watches the configured base URL. When it changes (including initial
 *    set), hydrates from the per-URL DataStore cache and triggers a background refresh.
 *  - Exposes the current config as a hot [StateFlow]; consumers can collect it or read
 *    `.value` directly. Null means "no config available yet" (no base URL configured,
 *    no cache, and any background fetch has not landed).
 *  - [refresh] forces a network round-trip and updates both the in-memory flow and the
 *    DataStore cache. Errors propagate to the caller; the cached value is left alone.
 */
class ServerConfigStore(
    private val api: ServerConfigApi,
    private val settings: SettingsStore,
    private val json: Json,
    scope: CoroutineScope,
) {
    private val _config = MutableStateFlow<ServerConfig?>(null)
    val config: StateFlow<ServerConfig?> = _config.asStateFlow()

    init {
        scope.launch {
            settings.baseUrl
                .distinctUntilChanged()
                .collect { url ->
                    _config.value = loadCached(url)
                    runCatching { refreshInternal(url) }
                }
        }
    }

    suspend fun refresh(): ServerConfig {
        return refreshInternal(settings.baseUrl.first())
    }

    private suspend fun loadCached(baseUrl: String): ServerConfig? {
        val raw = settings.cachedServerConfig(baseUrl) ?: return null
        return runCatching { json.decodeFromString<ServerConfig>(raw) }.getOrNull()
    }

    private suspend fun refreshInternal(baseUrl: String): ServerConfig {
        val fetched = api.fetch().unwrap()
        settings.setCachedServerConfig(baseUrl, json.encodeToString(ServerConfig.serializer(), fetched))
        _config.value = fetched
        return fetched
    }
}

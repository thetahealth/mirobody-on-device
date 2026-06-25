package ai.thetahealth.mirobody.data.settings

import android.content.Context
import androidx.datastore.preferences.core.Preferences
import androidx.datastore.preferences.core.edit
import androidx.datastore.preferences.core.intPreferencesKey
import androidx.datastore.preferences.core.stringPreferencesKey
import androidx.datastore.preferences.preferencesDataStore
import kotlinx.coroutines.flow.Flow
import kotlinx.coroutines.flow.first
import kotlinx.coroutines.flow.map

private val Context.dataStore by preferencesDataStore(name = "mirobody_settings")

class SettingsStore(private val context: Context) {

    // Falls back to the self-hosted server when the user has not configured a base URL,
    // so the app talks to the local server out of the box instead of failing.
    val baseUrl: Flow<String> = context.dataStore.data.map {
        it[KEY_BASE_URL]?.takeIf(String::isNotBlank) ?: DEFAULT_BASE_URL
    }
    val accessToken: Flow<String?> = context.dataStore.data.map { it[KEY_ACCESS_TOKEN] }
    val lastEmail: Flow<String?> = context.dataStore.data.map { it[KEY_LAST_EMAIL] }
    val language: Flow<String> = context.dataStore.data.map { it[KEY_LANGUAGE] ?: defaultLanguage() }
    val selectedProviderName: Flow<String?> = context.dataStore.data.map { it[KEY_SELECTED_PROVIDER_NAME] }
    val fontSizeOffset: Flow<Int> = context.dataStore.data.map { it[KEY_FONT_SIZE_OFFSET] ?: 0 }

    suspend fun setBaseUrl(value: String) {
        context.dataStore.edit { it[KEY_BASE_URL] = value.trim().trimEnd('/') }
    }

    suspend fun setAccessToken(token: String?) {
        context.dataStore.edit { prefs ->
            if (token.isNullOrBlank()) prefs.remove(KEY_ACCESS_TOKEN)
            else prefs[KEY_ACCESS_TOKEN] = token
        }
    }

    suspend fun setLastEmail(email: String?) {
        context.dataStore.edit { prefs ->
            if (email.isNullOrBlank()) prefs.remove(KEY_LAST_EMAIL)
            else prefs[KEY_LAST_EMAIL] = email
        }
    }

    suspend fun setLanguage(code: String) {
        context.dataStore.edit { it[KEY_LANGUAGE] = code }
    }

    suspend fun setSelectedProviderName(name: String?) {
        context.dataStore.edit { prefs ->
            if (name.isNullOrBlank()) prefs.remove(KEY_SELECTED_PROVIDER_NAME)
            else prefs[KEY_SELECTED_PROVIDER_NAME] = name
        }
    }

    suspend fun setFontSizeOffset(offset: Int) {
        context.dataStore.edit { it[KEY_FONT_SIZE_OFFSET] = offset }
    }

    suspend fun clearAuth() {
        context.dataStore.edit { prefs ->
            prefs.remove(KEY_ACCESS_TOKEN)
        }
    }

    /** Returns the cached server config JSON only if it was fetched from the given base URL. */
    suspend fun cachedServerConfig(baseUrl: String): String? {
        val prefs = context.dataStore.data.first()
        if (prefs[KEY_CACHED_CONFIG_URL] != baseUrl) return null
        return prefs[KEY_CACHED_CONFIG_JSON]
    }

    suspend fun setCachedServerConfig(baseUrl: String, configJson: String) {
        context.dataStore.edit { prefs ->
            prefs[KEY_CACHED_CONFIG_URL] = baseUrl
            prefs[KEY_CACHED_CONFIG_JSON] = configJson
        }
    }

    companion object {
        // Self-hosted server contacted when no base URL is configured (see baseUrl above).
        const val DEFAULT_BASE_URL: String = "http://localhost:8080"

        private val KEY_BASE_URL: Preferences.Key<String> = stringPreferencesKey("base_url")
        private val KEY_ACCESS_TOKEN: Preferences.Key<String> = stringPreferencesKey("access_token")
        private val KEY_LAST_EMAIL: Preferences.Key<String> = stringPreferencesKey("last_email")
        private val KEY_LANGUAGE: Preferences.Key<String> = stringPreferencesKey("language")
        private val KEY_SELECTED_PROVIDER_NAME: Preferences.Key<String> = stringPreferencesKey("selected_provider_name")
        private val KEY_FONT_SIZE_OFFSET: Preferences.Key<Int> = intPreferencesKey("font_size_offset")
        private val KEY_CACHED_CONFIG_URL: Preferences.Key<String> = stringPreferencesKey("cached_config_url")
        private val KEY_CACHED_CONFIG_JSON: Preferences.Key<String> = stringPreferencesKey("cached_config_json")

        // Picks device locale if it's one we expose, else falls back to English.
        // "iw" is Hebrew (Java's normalized code; Locale.getDefault().language
        // returns "iw" on a Hebrew device).
        private val SUPPORTED = setOf("zh", "ja", "ko", "en", "fr", "de", "ru", "es", "ar", "iw")
        fun defaultLanguage(): String {
            val sys = java.util.Locale.getDefault().language
            return if (sys in SUPPORTED) sys else "en"
        }
    }
}
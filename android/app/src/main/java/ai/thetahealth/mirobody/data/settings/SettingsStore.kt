package ai.thetahealth.mirobody.data.settings

import android.content.Context
import android.util.Base64
import androidx.datastore.preferences.core.Preferences
import androidx.datastore.preferences.core.edit
import androidx.datastore.preferences.core.intPreferencesKey
import androidx.datastore.preferences.core.stringPreferencesKey
import androidx.datastore.preferences.preferencesDataStore
import kotlinx.coroutines.flow.Flow
import kotlinx.coroutines.flow.first
import kotlinx.coroutines.flow.map
import org.json.JSONObject

private val Context.dataStore by preferencesDataStore(name = "mirobody_settings")

/** A stored account for the switcher: its opaque `sub`, its email (may be blank
 *  for a social login that returned none), and whether it's the active one. */
data class StoredAccount(val sub: String, val email: String, val current: Boolean)

class SettingsStore(private val context: Context) {

    // Falls back to the self-hosted server when the user has not configured a base URL,
    // so the app talks to the local server out of the box instead of failing.
    val baseUrl: Flow<String> = context.dataStore.data.map {
        it[KEY_BASE_URL]?.takeIf(String::isNotBlank) ?: DEFAULT_BASE_URL
    }
    // The CURRENT account's token. Multi-account: each account's JWT lives under
    // "access_token_<sub>" and KEY_CURRENT_SUB names the active one, so several
    // accounts coexist on one device. Falls back to the legacy single-token slot
    // until migrateLegacyIfNeeded() converts it. tokenFlow / AuthInterceptor / the
    // nav graph all read this unchanged -- they just see the active account.
    val accessToken: Flow<String?> = context.dataStore.data.map { currentToken(it) }
    val lastEmail: Flow<String?> = context.dataStore.data.map { it[KEY_LAST_EMAIL] }

    // The active account's email, decoded from the token's `email` claim (the
    // mb_oauth access token carries it). Null when signed out or the token has none.
    val currentEmail: Flow<String?> = context.dataStore.data.map { prefs ->
        currentToken(prefs)?.let { emailOf(it) }
    }

    // Every stored account (current first) for the drawer's account switcher.
    val accounts: Flow<List<StoredAccount>> = context.dataStore.data.map { prefs ->
        val cur = prefs[KEY_CURRENT_SUB]
        prefs.asMap().entries
            .filter { it.key.name.startsWith(TOKEN_PREFIX) }
            .map { e ->
                val sub = e.key.name.removePrefix(TOKEN_PREFIX)
                StoredAccount(sub, (e.value as? String)?.let { emailOf(it) } ?: "", sub == cur)
            }
            .sortedByDescending { it.current }
    }
    val language: Flow<String> = context.dataStore.data.map { it[KEY_LANGUAGE] ?: defaultLanguage() }
    val selectedProviderName: Flow<String?> = context.dataStore.data.map { it[KEY_SELECTED_PROVIDER_NAME] }
    val fontSizeOffset: Flow<Int> = context.dataStore.data.map { it[KEY_FONT_SIZE_OFFSET] ?: 0 }

    suspend fun setBaseUrl(value: String) {
        context.dataStore.edit { it[KEY_BASE_URL] = value.trim().trimEnd('/') }
    }

    // Store a token under its own account slot and make it current. Dedupes by
    // email: the server re-salts `sub` per issuance, so the same account re-logging
    // in gets a new sub -- drop any existing slot with the same email first, so
    // there's one entry per account rather than one per login. A blank token signs
    // the current account out (clearAuth).
    suspend fun setAccessToken(token: String?) {
        if (token.isNullOrBlank()) { clearAuth(); return }
        val sub = subOf(token) ?: return
        val email = emailOf(token)
        context.dataStore.edit { prefs ->
            if (!email.isNullOrBlank()) {
                prefs.asMap().keys
                    .filter { it.name.startsWith(TOKEN_PREFIX) && it.name != TOKEN_PREFIX + sub }
                    .toList()
                    .forEach { k ->
                        @Suppress("UNCHECKED_CAST")
                        val other = prefs[k as Preferences.Key<String>]
                        if (other != null && emailOf(other) == email) prefs.remove(k)
                    }
            }
            prefs[tokenKey(sub)] = token
            prefs[KEY_CURRENT_SUB] = sub
            prefs.remove(KEY_ACCESS_TOKEN)   // legacy slot no longer used
        }
    }

    /** Make an already-stored account current (no-op if its slot is gone). */
    suspend fun switchAccount(sub: String) {
        context.dataStore.edit { prefs ->
            if (prefs[tokenKey(sub)] != null) prefs[KEY_CURRENT_SUB] = sub
        }
    }

    /** True when at least one account is still stored (used after signing one out). */
    suspend fun hasAccount(): Boolean =
        context.dataStore.data.first().let { currentToken(it) != null }

    /** The active account's `sub`, used to namespace per-account local data. */
    suspend fun currentSub(): String? = context.dataStore.data.first()[KEY_CURRENT_SUB]

    // Convert the old single-token slot into the per-account scheme, once.
    suspend fun migrateLegacyIfNeeded() {
        context.dataStore.edit { prefs ->
            val legacy = prefs[KEY_ACCESS_TOKEN] ?: return@edit
            if (prefs[KEY_CURRENT_SUB] == null) {
                val sub = subOf(legacy)
                if (sub != null) {
                    prefs[tokenKey(sub)] = legacy
                    prefs[KEY_CURRENT_SUB] = sub
                }
            }
            prefs.remove(KEY_ACCESS_TOKEN)
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

    // Sign out the CURRENT account: drop its slot, then fall back to another
    // stored account if one exists (stay signed in as it), else clear the pointer
    // (signed out -> tokenFlow emits null -> the nav graph returns to login).
    suspend fun clearAuth() {
        context.dataStore.edit { prefs ->
            prefs[KEY_CURRENT_SUB]?.let { prefs.remove(tokenKey(it)) }
            val next = prefs.asMap().keys
                .map { it.name }
                .firstOrNull { it.startsWith(TOKEN_PREFIX) }
            if (next != null) prefs[KEY_CURRENT_SUB] = next.removePrefix(TOKEN_PREFIX)
            else prefs.remove(KEY_CURRENT_SUB)
            prefs.remove(KEY_ACCESS_TOKEN)
        }
    }

    // ---- JWT / per-account helpers ---------------------------------------
    private fun tokenKey(sub: String): Preferences.Key<String> =
        stringPreferencesKey(TOKEN_PREFIX + sub)

    private fun currentToken(prefs: Preferences): String? {
        val sub = prefs[KEY_CURRENT_SUB]
        // Pre-migration reads fall back to the legacy single-token slot.
        return if (sub != null) prefs[tokenKey(sub)] else prefs[KEY_ACCESS_TOKEN]
    }

    private fun subOf(token: String): String? =
        decodePayload(token)?.optString("sub")?.takeIf(String::isNotBlank)

    private fun emailOf(token: String): String? =
        decodePayload(token)?.optString("email")?.takeIf(String::isNotBlank)

    // Decode a JWT's payload (middle segment) to a JSON object, or null if the
    // token is missing/malformed. Base64url, no padding.
    private fun decodePayload(token: String): JSONObject? = runCatching {
        val parts = token.split(".")
        if (parts.size < 2) return null
        val bytes = Base64.decode(parts[1], Base64.URL_SAFE or Base64.NO_PADDING or Base64.NO_WRAP)
        JSONObject(String(bytes, Charsets.UTF_8))
    }.getOrNull()

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
        // Legacy single-token slot, migrated to the per-account scheme on launch.
        private val KEY_ACCESS_TOKEN: Preferences.Key<String> = stringPreferencesKey("access_token")
        // Per-account tokens live under "access_token_<sub>"; KEY_CURRENT_SUB names
        // the active account.
        private const val TOKEN_PREFIX: String = "access_token_"
        private val KEY_CURRENT_SUB: Preferences.Key<String> = stringPreferencesKey("current_sub")
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

        // A synchronous mirror of the chosen language in plain SharedPreferences.
        // The language lives in DataStore (async), but Activity.attachBaseContext
        // must set the app locale synchronously before onCreate, so MainActivity
        // reads it here and mirrors any change back (see MainActivity).
        private const val LOCALE_PREFS = "mirobody_locale"
        private const val KEY_PERSISTED_LANG = "language"

        fun persistedLanguage(context: Context): String =
            context.getSharedPreferences(LOCALE_PREFS, Context.MODE_PRIVATE)
                .getString(KEY_PERSISTED_LANG, null) ?: defaultLanguage()

        fun setPersistedLanguage(context: Context, code: String) {
            context.getSharedPreferences(LOCALE_PREFS, Context.MODE_PRIVATE)
                .edit().putString(KEY_PERSISTED_LANG, code).apply()
        }
    }
}
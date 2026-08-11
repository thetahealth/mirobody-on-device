package ai.thetahealth.mirobody.data.chat

import ai.thetahealth.mirobody.data.settings.SettingsStore
import ai.thetahealth.mirobody.ui.chat.ChatMessage
import ai.thetahealth.mirobody.ui.chat.Role
import android.content.Context
import androidx.datastore.preferences.core.edit
import androidx.datastore.preferences.core.stringPreferencesKey
import androidx.datastore.preferences.preferencesDataStore
import kotlinx.coroutines.flow.first
import kotlinx.coroutines.flow.map
import kotlinx.serialization.decodeFromString
import kotlinx.serialization.encodeToString
import kotlinx.serialization.json.Json

private val Context.chatHistoryDataStore by preferencesDataStore(name = "mirobody_chat_history")

/**
 * Local persistence for the running chat conversation, so a relaunch restores it
 * instead of starting blank. Kept per account -- the message key is namespaced by
 * the current account's `sub` -- so switching accounts shows that account's own
 * conversation and signing one out clears only its slot. JSON-encoded; everything
 * is best-effort -- a decode/encode failure yields an empty history, not a throw.
 */
class ChatHistoryStore(context: Context, private val settings: SettingsStore, private val json: Json) {

    private val appContext = context.applicationContext

    // Per-account key ("messages_<sub>"); "messages" (no suffix) when signed out.
    private suspend fun messagesKey() =
        stringPreferencesKey("messages" + (settings.currentSub()?.let { "_$it" } ?: ""))

    /** The stored conversation, or empty when nothing is saved / it can't be read. */
    suspend fun load(): List<ChatMessage> {
        val key = messagesKey()
        val raw = appContext.chatHistoryDataStore.data
            .map { it[key] }
            .first() ?: return emptyList()
        return runCatching { json.decodeFromString<List<ChatMessage>>(raw) }
            .getOrDefault(emptyList())
    }

    /**
     * Replace the stored conversation. Only settled turns are kept: the in-flight
     * (streaming) assistant placeholder and any empty assistant bubble are dropped
     * so a restore shows real content, not a dangling cursor.
     */
    suspend fun save(messages: List<ChatMessage>) {
        val settled = messages.filter { msg ->
            // A `local` message was produced by the composer (a slash command), not by a
            // turn. It is shown for as long as the screen lives and never stored.
            !msg.local && !msg.streaming && (
                msg.role == Role.User ||
                    msg.text.isNotEmpty() ||
                    msg.toolCalls.isNotEmpty() ||
                    msg.imageUrls.isNotEmpty() ||
                    msg.error != null
            )
        }
        val raw = runCatching { json.encodeToString(settled) }.getOrNull() ?: return
        val key = messagesKey()
        appContext.chatHistoryDataStore.edit { it[key] = raw }
    }

    /** Drop the current account's stored conversation (e.g. on sign-out). */
    suspend fun clear() {
        val key = messagesKey()
        appContext.chatHistoryDataStore.edit { it.remove(key) }
    }
}

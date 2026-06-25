package ai.thetahealth.mirobody.data.chat

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
 * instead of starting blank. One conversation is kept per install (its own
 * DataStore, separate from settings) as a JSON-encoded message list; cleared on
 * sign-out. Everything is best-effort -- a decode/encode failure yields an empty
 * history rather than throwing.
 */
class ChatHistoryStore(context: Context, private val json: Json) {

    private val appContext = context.applicationContext

    /** The stored conversation, or empty when nothing is saved / it can't be read. */
    suspend fun load(): List<ChatMessage> {
        val raw = appContext.chatHistoryDataStore.data
            .map { it[KEY_MESSAGES] }
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
            !msg.streaming && (
                msg.role == Role.User ||
                    msg.text.isNotEmpty() ||
                    msg.toolCalls.isNotEmpty() ||
                    msg.imageUrls.isNotEmpty() ||
                    msg.error != null
            )
        }
        val raw = runCatching { json.encodeToString(settled) }.getOrNull() ?: return
        appContext.chatHistoryDataStore.edit { it[KEY_MESSAGES] = raw }
    }

    /** Drop the stored conversation. */
    suspend fun clear() {
        appContext.chatHistoryDataStore.edit { it.remove(KEY_MESSAGES) }
    }

    private companion object {
        val KEY_MESSAGES = stringPreferencesKey("messages")
    }
}

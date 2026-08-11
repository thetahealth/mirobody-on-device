package ai.thetahealth.mirobody.data.chat

import ai.thetahealth.mirobody.data.chat.dto.ChatAttachment
import ai.thetahealth.mirobody.data.chat.dto.ChatStreamEvent
import ai.thetahealth.mirobody.data.chat.dto.ChatStreamRequest
import ai.thetahealth.mirobody.data.chat.dto.ConversationDetail
import ai.thetahealth.mirobody.data.chat.dto.HistoryDeleteRequest
import ai.thetahealth.mirobody.data.chat.dto.ProviderInfo
import ai.thetahealth.mirobody.data.chat.dto.SessionSummary
import ai.thetahealth.mirobody.data.llm.ChatTurn
import ai.thetahealth.mirobody.data.llm.OnDeviceLlmEngine
import ai.thetahealth.mirobody.data.llm.OnDeviceModelSpec
import ai.thetahealth.mirobody.data.net.ensureOk
import ai.thetahealth.mirobody.data.net.unwrap
import kotlinx.coroutines.flow.Flow

class ChatRepository(
    private val api: ChatApi,
    private val streamClient: ChatStreamClient,
    private val onDeviceEngine: OnDeviceLlmEngine,
) {
    suspend fun listProviders(): List<ProviderInfo> =
        api.listProviders().unwrap().flatMap { g ->
            g.providers.map { ProviderInfo(agent = g.agent, provider = it) }
        }

    suspend fun history(page: Int = 0, pageSize: Int = 20): List<SessionSummary> =
        api.history(page = page, pageSize = pageSize).unwrap().summaries

    suspend fun deleteHistory(sessionId: String) {
        api.deleteHistory(HistoryDeleteRequest(sessionId = sessionId)).ensureOk()
    }

    suspend fun conversation(sessionId: String): ConversationDetail =
        api.conversation(sessionId).unwrap()

    fun chat(
        sessionId: String,
        question: String,
        agent: String,
        provider: String,
        language: String,
        subject: Long = 0,
        attachments: List<ChatAttachment> = emptyList(),
        incognito: Boolean = false,
    ): Flow<ChatStreamEvent> = streamClient.stream(
        ChatStreamRequest(
            question = question,
            sessionId = sessionId,
            agent = agent,
            provider = provider,
            language = language,
            subject = if (subject > 0) subject.toString() else null,
            incognito = incognito,
        ),
        attachments = attachments,
    )

    /**
     * Local, offline turn: routes to the on-device engine ([model]) instead of the
     * server. [history]'s final entry is the new user question; earlier entries are
     * conversation context. Emits the same [ChatStreamEvent] flow the SSE path does.
     */
    fun chatOnDevice(history: List<ChatTurn>, model: OnDeviceModelSpec): Flow<ChatStreamEvent> =
        onDeviceEngine.generate(history, model)

    /** Warm an on-device model so the first turn does not pay for its load. */
    suspend fun preloadOnDevice(model: OnDeviceModelSpec) = onDeviceEngine.preload(model)
}

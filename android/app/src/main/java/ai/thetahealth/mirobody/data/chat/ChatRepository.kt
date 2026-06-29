package ai.thetahealth.mirobody.data.chat

import ai.thetahealth.mirobody.data.chat.dto.ChatAttachment
import ai.thetahealth.mirobody.data.chat.dto.ChatStreamEvent
import ai.thetahealth.mirobody.data.chat.dto.ChatStreamRequest
import ai.thetahealth.mirobody.data.chat.dto.HistoryDeleteRequest
import ai.thetahealth.mirobody.data.chat.dto.ProviderInfo
import ai.thetahealth.mirobody.data.chat.dto.SessionSummary
import ai.thetahealth.mirobody.data.net.ensureOk
import ai.thetahealth.mirobody.data.net.unwrap
import kotlinx.coroutines.flow.Flow

class ChatRepository(
    private val api: ChatApi,
    private val streamClient: ChatStreamClient,
) {
    suspend fun listProviders(): List<ProviderInfo> =
        api.listProviders().unwrap()

    suspend fun history(page: Int = 0, pageSize: Int = 20): List<SessionSummary> =
        api.history(page = page, pageSize = pageSize).unwrap().summaries

    suspend fun deleteHistory(sessionId: String) {
        api.deleteHistory(HistoryDeleteRequest(sessionId = sessionId)).ensureOk()
    }

    fun chat(
        sessionId: String,
        question: String,
        agentCode: String,
        provider: String,
        language: String,
        subject: Long = 0,
        attachments: List<ChatAttachment> = emptyList(),
    ): Flow<ChatStreamEvent> = streamClient.stream(
        ChatStreamRequest(
            question = question,
            sessionId = sessionId,
            agent = agentCode,
            provider = provider,
            language = language,
            subject = if (subject > 0) subject.toString() else null,
        ),
        attachments = attachments,
    )
}

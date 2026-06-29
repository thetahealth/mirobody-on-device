package ai.thetahealth.mirobody.data.chat.dto

import kotlinx.serialization.SerialName
import kotlinx.serialization.Serializable

@Serializable
data class HistoryResponse(
    val summaries: List<SessionSummary> = emptyList(),
)

@Serializable
data class SessionSummary(
    @SerialName("session_id") val sessionId: String = "",
    val timestamp: Long = 0L,
    val summary: String = "",
    @SerialName("query_user_id") val queryUserId: String = "",
    val owned: Boolean = true,
    @SerialName("shared_with_count") val sharedWithCount: Int = 0,
)

@Serializable
data class HistoryDeleteRequest(
    @SerialName("session_id") val sessionId: String,
)
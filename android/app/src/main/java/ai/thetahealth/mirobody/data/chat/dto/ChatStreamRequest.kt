package ai.thetahealth.mirobody.data.chat.dto

import kotlinx.serialization.SerialName
import kotlinx.serialization.Serializable

@Serializable
data class ChatStreamRequest(
    val question: String,
    @SerialName("session_id") val sessionId: String,
    val agent: String,
    val provider: String = "",
    @SerialName("enable_mcp") val enableMcp: Int = 1,
    @SerialName("file_list") val fileList: List<FileRef> = emptyList(),
    @SerialName("prompt_name") val promptName: String = "",
    val language: String = "en",
    val timezone: String = "Asia/Shanghai",
    val scene: String? = null,
    // Opaque care-circle member handle for the "currently for" subject (whose
    // health the AI's family_health tool should default to). Null/omitted = self.
    val subject: String? = null,
    // Privacy mode: when true the server persists nothing for this turn and
    // disables memory (src/chat/params.cpp). Omitted when false (encodeDefaults off).
    val incognito: Boolean = false,
)

@Serializable
data class FileRef(
    @SerialName("file_key") val fileKey: String = "",
    @SerialName("file_type") val fileType: String = "",
    @SerialName("file_name") val fileName: String = "",
    @SerialName("file_url") val fileUrl: String = "",
    @SerialName("file_size") val fileSize: Long = 0,
    val duration: Long = 0,
    @SerialName("storage_key") val storageKey: String = "",
    val url: String = "",
)
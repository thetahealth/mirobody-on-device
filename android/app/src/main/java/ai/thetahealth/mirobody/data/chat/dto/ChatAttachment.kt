package ai.thetahealth.mirobody.data.chat.dto

/**
 * A file the user attached to a chat turn, read into memory at pick time. Sent as a
 * multipart `file` part to /api/chat (mirrors the web client's FormData upload); the
 * server stores it to object storage and references it on the turn. [bytes] equality
 * is intentionally by reference — attachments are short-lived, identified by list
 * position in the composer.
 */
data class ChatAttachment(
    val fileName: String,
    val mimeType: String,
    val bytes: ByteArray,
)

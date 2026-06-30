package ai.thetahealth.mirobody.data.llm

import ai.thetahealth.mirobody.data.chat.dto.ChatStreamEvent
import kotlinx.coroutines.flow.Flow

/** One settled turn of a conversation, fed to the on-device model as context. */
data class ChatTurn(val fromUser: Boolean, val text: String)

/**
 * A locally-hosted LLM that produces the same [ChatStreamEvent] stream the remote
 * `/api/chat` SSE path does, so the existing chat UI renders on-device replies with
 * no UI changes. Implementations run fully on-device — no network, no server.
 *
 * The Android implementation is [LiteRtLlmEngine] (Gemma 4 via LiteRT-LM). The
 * interface is deliberately runtime-agnostic so an ML Kit GenAI (Gemini Nano) path
 * can slot in later behind the same seam.
 */
interface OnDeviceLlmEngine {
    /**
     * Stream a reply for [history]; the final entry is the new user turn. Emits
     * [ChatStreamEvent.Reply] deltas (and optional [ChatStreamEvent.Thinking]),
     * then [ChatStreamEvent.End]. Failures surface as [ChatStreamEvent.Error]
     * rather than throwing, mirroring the SSE client's contract.
     */
    fun generate(history: List<ChatTurn>): Flow<ChatStreamEvent>

    /** Release native resources (the model file stays on disk). Safe to call repeatedly. */
    fun close()
}

package ai.thetahealth.mirobody.data.llm

import ai.thetahealth.mirobody.data.chat.dto.ChatStreamEvent
import com.google.ai.edge.litertlm.Backend
import com.google.ai.edge.litertlm.Contents
import com.google.ai.edge.litertlm.Conversation
import com.google.ai.edge.litertlm.ConversationConfig
import com.google.ai.edge.litertlm.Engine
import com.google.ai.edge.litertlm.EngineConfig
import com.google.ai.edge.litertlm.SamplerConfig
import kotlinx.coroutines.flow.Flow
import kotlinx.coroutines.flow.flow
import kotlinx.coroutines.sync.Mutex
import kotlinx.coroutines.sync.withLock

/**
 * On-device LLM backed by Gemma 4 (E2B) via LiteRT-LM (`com.google.ai.edge.litertlm`).
 * Fully offline: loads the `.litertlm` file [ModelManager] downloaded and streams tokens.
 *
 * Symbol names (Engine / EngineConfig / Conversation / sendMessageAsync) track the
 * LiteRT-LM Kotlin getting-started guide; confirm against the pinned `litertlm-android`
 * version when wiring the build:
 *   https://github.com/google-ai-edge/LiteRT-LM/blob/main/docs/api/kotlin/getting_started.md
 *
 * The engine (model weights in memory) is created lazily on first use and reused; it is
 * expensive to initialize (several seconds, ~3 GB RAM). A single [Conversation] carries
 * multi-turn state; a turn whose history has collapsed to just the new question (a fresh
 * chat) resets it.
 */
class LiteRtLlmEngine(
    private val models: ModelManager,
) : OnDeviceLlmEngine {

    private val initLock = Mutex()
    @Volatile private var engine: Engine? = null
    @Volatile private var conversation: Conversation? = null

    private suspend fun engine(): Engine = engine ?: initLock.withLock {
        engine ?: Engine(
            EngineConfig(
                modelPath = models.modelFile.absolutePath,
                // CPU is the safe default across devices; GPU/NPU can be opted into later
                // once we gate on device capability (Backend.GPU()/Backend.NPU(...)).
                backend = Backend.CPU(),
                cacheDir = models.modelFile.parentFile?.absolutePath,
            ),
        ).also {
            it.initialize()
            engine = it
        }
    }

    override fun generate(history: List<ChatTurn>): Flow<ChatStreamEvent> = flow {
        if (!models.isReady()) {
            emit(ChatStreamEvent.Error("On-device model not downloaded yet."))
            emit(ChatStreamEvent.End)
            return@flow
        }

        val engine = engine()
        val question = history.lastOrNull { it.fromUser }?.text.orEmpty()
        if (question.isBlank()) {
            emit(ChatStreamEvent.End)
            return@flow
        }

        // Fresh chat (only the new question present) → drop prior conversation state.
        if (history.count() <= 1) {
            conversation?.close()
            conversation = null
        }
        val convo = conversation ?: engine.createConversation(
            ConversationConfig(
                // Seed any restored transcript (everything before the new question) as
                // context so a relaunched session keeps continuity without re-generating.
                systemInstruction = Contents.of(buildSystemInstruction(history.dropLast(1))),
                samplerConfig = SamplerConfig(topK = 40, topP = 0.95, temperature = 0.8),
            ),
        ).also { conversation = it }

        // Stream tokens. The SDK emits cumulative-or-delta Message objects depending on
        // version; we diff against what we've already emitted so the UI always receives
        // pure deltas (matching the SSE `reply` contract).
        var emitted = ""
        runCatching {
            convo.sendMessageAsync(question).collect { message ->
                val full = message.toString()
                val delta = if (full.startsWith(emitted)) full.substring(emitted.length) else full
                if (delta.isNotEmpty()) {
                    emitted = if (full.startsWith(emitted)) full else emitted + full
                    emit(ChatStreamEvent.Reply(delta))
                }
            }
        }.onFailure { t ->
            emit(ChatStreamEvent.Error(t.message ?: "On-device generation failed"))
        }
        emit(ChatStreamEvent.End)
    }

    private fun buildSystemInstruction(prior: List<ChatTurn>): String {
        val base = "You are Mirobody's private on-device health assistant. " +
            "Answer concisely. You have no internet or tools; rely only on the conversation."
        if (prior.isEmpty()) return base
        val transcript = prior.joinToString("\n") { turn ->
            val who = if (turn.fromUser) "User" else "Assistant"
            "$who: ${turn.text}"
        }
        return "$base\n\nConversation so far:\n$transcript"
    }

    override fun close() {
        conversation?.close()
        conversation = null
        engine?.close()
        engine = null
    }
}

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
 * On-device LLM via LiteRT-LM (`com.google.ai.edge.litertlm`). Fully offline: loads a
 * downloaded `.litertlm` file (any model in [OnDeviceModel.CATALOG]) and streams tokens.
 *
 * Symbol names (Engine / EngineConfig / Conversation / sendMessageAsync) track the
 * LiteRT-LM Kotlin getting-started guide; confirm against the pinned `litertlm-android`
 * version when wiring the build:
 *   https://github.com/google-ai-edge/LiteRT-LM/blob/main/docs/api/kotlin/getting_started.md
 *
 * The engine (model weights in memory) is created lazily on first use and reused; it is
 * expensive to initialize (several seconds, ~3 GB RAM). Switching to a different model
 * tears the current engine down and loads the new one. A single [Conversation] carries
 * multi-turn state; a turn whose history has collapsed to just the new question (a fresh
 * chat) resets it.
 */
class LiteRtLlmEngine(
    private val models: ModelManager,
) : OnDeviceLlmEngine {

    private val initLock = Mutex()
    @Volatile private var engine: Engine? = null
    @Volatile private var conversation: Conversation? = null
    // Which model the live engine was loaded from; a different one forces a reload.
    @Volatile private var loadedModelId: String? = null

    private suspend fun engine(model: OnDeviceModelSpec): Engine = initLock.withLock {
        val current = engine
        if (current != null && loadedModelId == model.id) return@withLock current
        // Different (or first) model: drop any live engine/conversation and load fresh.
        conversation?.close(); conversation = null
        engine?.close()
        val file = models.fileFor(model)
        Engine(
            EngineConfig(
                modelPath = file.absolutePath,
                // CPU is the safe default across devices; GPU/NPU can be opted into later
                // once we gate on device capability (Backend.GPU()/Backend.NPU(...)).
                backend = Backend.CPU(),
                // Cap total context so the native layer allocates a bounded KV cache and
                // stops cleanly instead of over-running (a likely cause of mid-generation
                // native crashes). Kept small for on-device RAM; fits every catalog model.
                maxNumTokens = 1280,
                cacheDir = file.parentFile?.absolutePath,
            ),
        ).also {
            it.initialize()
            engine = it
            loadedModelId = model.id
        }
    }

    override fun generate(history: List<ChatTurn>, model: OnDeviceModelSpec): Flow<ChatStreamEvent> = flow {
        if (!models.isReady(model)) {
            emit(ChatStreamEvent.Error("On-device model not downloaded yet."))
            emit(ChatStreamEvent.End)
            return@flow
        }

        val engine = engine(model)
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
        // NB: don't seed the prior transcript into the system prompt — on small on-device
        // models the context window is tiny (e.g. Qwen3 0.6B is 2048 tokens), and a
        // restored/long history overflows it. Multi-turn within a session is carried by
        // the reused Conversation itself; the system prompt stays a short, fixed instruction.
        val convo = conversation ?: engine.createConversation(
            ConversationConfig(
                systemInstruction = Contents.of(SYSTEM_PROMPT),
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

    private companion object {
        const val SYSTEM_PROMPT = "You are Mirobody's private on-device health assistant. " +
            "Answer concisely. You have no internet or tools; rely only on the conversation."
    }

    override fun close() {
        conversation?.close()
        conversation = null
        engine?.close()
        engine = null
        loadedModelId = null
    }
}

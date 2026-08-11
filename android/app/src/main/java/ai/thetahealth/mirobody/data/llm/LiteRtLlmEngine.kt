package ai.thetahealth.mirobody.data.llm

import ai.thetahealth.mirobody.data.chat.dto.ChatStreamEvent
import com.google.ai.edge.litertlm.Backend
import com.google.ai.edge.litertlm.Contents
import com.google.ai.edge.litertlm.Conversation
import com.google.ai.edge.litertlm.ConversationConfig
import com.google.ai.edge.litertlm.Engine
import com.google.ai.edge.litertlm.EngineConfig
import com.google.ai.edge.litertlm.SamplerConfig
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.withContext
import kotlinx.coroutines.flow.Flow
import kotlinx.coroutines.flow.flowOn
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

    /** Is the live engine already holding this model? Then the turn starts immediately. */
    private fun isLoaded(model: OnDeviceModelSpec): Boolean =
        engine != null && loadedModelId == model.id

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
                // The backend comes from the SPEC, because it is a property of the file:
                // a *-gpu.litertlm is compiled for the GPU and the plain one is not.
                // Nothing here picks an accelerator on the user's behalf — the catalog
                // ships CPU builds, and the GPU twin is downloaded deliberately from the
                // debug probe (OnDeviceModel.DEBUG_MODELS).
                backend = when (model.backend) {
                    OnDeviceBackend.CPU -> Backend.CPU()
                    OnDeviceBackend.GPU -> Backend.GPU()
                },
                // Cap total context so the native layer allocates a bounded KV cache and
                // stops cleanly instead of over-running (a likely cause of mid-generation
                // native crashes). Kept small for on-device RAM; fits every catalog model.
                maxNumTokens = 1280,
                // App-private and per-model, NOT beside the weights: this cache is
                // model-sized and regenerable. See ModelManager.cacheDirFor.
                cacheDir = models.cacheDirFor(model).absolutePath,
            ),
        ).also {
            it.initialize()
            engine = it
            loadedModelId = model.id
        }
    }

    /**
     * Warm the engine ahead of the first question.
     *
     * Nothing here is free — engine() does the same seconds of work it always does — but
     * it happens while the user is still deciding what to ask. Silent on failure: this is
     * an optimisation, and a model that cannot load will say so properly when a turn
     * actually asks for it.
     */
    override suspend fun preload(model: OnDeviceModelSpec) {
        if (isLoaded(model) || !models.isReady(model)) return
        withContext(Dispatchers.Default) { runCatching { engine(model) } }
    }

    override fun generate(history: List<ChatTurn>, model: OnDeviceModelSpec): Flow<ChatStreamEvent> = flow {
        if (!models.isReady(model)) {
            emit(ChatStreamEvent.Error("On-device model not downloaded yet."))
            emit(ChatStreamEvent.End)
            return@flow
        }

        // Announce the load BEFORE starting it: creating the engine reads a few GB and
        // takes seconds, and without this the turn looks like a model already writing.
        if (!isLoaded(model)) emit(ChatStreamEvent.Loading)
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
        // NB: don't seed the prior transcript into the system prompt — an on-device
        // build's KV budget is small (litert-community publishes these at around 4096
        // tokens), and a restored/long history overflows it. Multi-turn within a session
        // is carried by the reused Conversation itself; the system prompt stays a short,
        // fixed instruction.
        val convo = conversation ?: engine.createConversation(
            ConversationConfig(
                systemInstruction = Contents.of(SYSTEM_PROMPT),
                samplerConfig = SamplerConfig(topK = 40, topP = 0.95, temperature = 0.8),
            ),
        ).also { conversation = it }

        // Stream tokens. The SDK emits cumulative-or-delta Message objects depending on
        // version; we diff against what we've already emitted so the UI always receives
        // pure deltas (matching the SSE `reply` contract).
        //
        // Those deltas then go through ThinkSplitter, because a reasoning model (Qwen3 is
        // one) writes its reasoning inline as <think>…</think> and the reply proper only
        // begins after the close tag. Routing it to Thinking rather than Reply is what
        // puts it in the collapsible block the UI already has for the server's `thinking`
        // event — and what keeps the tags from showing up as prose.
        var emitted = ""
        val split = ThinkSplitter()
        runCatching {
            convo.sendMessageAsync(question).collect { message ->
                val full = message.toString()
                val delta = if (full.startsWith(emitted)) full.substring(emitted.length) else full
                if (delta.isNotEmpty()) {
                    emitted = if (full.startsWith(emitted)) full else emitted + full
                    for (p in split.feed(delta)) {
                        emit(if (p.thinking) ChatStreamEvent.Thinking(p.text) else ChatStreamEvent.Reply(p.text))
                    }
                }
            }
        }.onFailure { t ->
            emit(ChatStreamEvent.Error(t.message ?: "On-device generation failed"))
        }
        // Whatever the splitter is still holding was a tag prefix that never completed;
        // it is text, and dropping it would eat the end of the reply.
        for (p in split.flush()) {
            emit(if (p.thinking) ChatStreamEvent.Thinking(p.text) else ChatStreamEvent.Reply(p.text))
        }
        emit(ChatStreamEvent.End)
    }
        // THE WHOLE FLOW RUNS OFF THE MAIN THREAD. Without this it does not: a bare
        // flow { } builder runs in the COLLECTOR's context, and the collector is
        // viewModelScope — Dispatchers.Main. Engine.initialize() blocks for seconds
        // over a few GB and sendMessageAsync's collection blocks per token, so the
        // first on-device turn froze the UI outright: not a missing spinner, a blocked
        // main thread, which is also why the typing dots could not animate through it.
        //
        // Default rather than IO: past the initial read this is compute, and the native
        // layer runs its own threads either way. flowOn moves the upstream only —
        // emissions still arrive on the collector's context, so the ViewModel is
        // unchanged.
        .flowOn(Dispatchers.Default)

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

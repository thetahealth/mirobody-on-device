package ai.thetahealth.mirobody.data.llm

import ai.thetahealth.mirobody.NativeBridge
import ai.thetahealth.mirobody.data.chat.dto.ChatStreamEvent
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.channels.Channel
import kotlinx.coroutines.flow.Flow
import kotlinx.coroutines.flow.buffer
import kotlinx.coroutines.flow.channelFlow
import kotlinx.coroutines.flow.flowOn
import kotlinx.coroutines.isActive
import kotlinx.coroutines.sync.Mutex
import kotlinx.coroutines.sync.withLock
import kotlinx.coroutines.withContext

/**
 * On-device LLM via llama.cpp, for the GGUF half of [OnDeviceModel.CATALOG].
 *
 * The engine itself is C++ (`src/llm/local.cpp`, the same client HarmonyOS and Qt use);
 * this is the Android seam onto it, reached through [NativeBridge]. Everything that has
 * an opinion — the context ladder, sampling, the `<think>` split, the sliding history
 * window — lives on that side, so all three frontends behave the same and a fix lands
 * once. What is left here is lifecycle: which model is loaded, and one turn at a time.
 *
 * The native handle owns the weights for its life, because loading them is seconds and a
 * few GB; switching models tears it down and opens another. Unlike the LiteRT path there
 * is no conversation object — every turn replays the transcript and C++ trims it to fit,
 * which is why history is passed in full on each call.
 */
class LlamaCppEngine(
    private val models: ModelManager,
    private val bridge: NativeBridge = NativeBridge(),
) : OnDeviceLlmEngine {

    private val initLock = Mutex()
    @Volatile private var handle: Long = 0L
    @Volatile private var loadedModelId: String? = null

    private fun isLoaded(model: OnDeviceModelSpec): Boolean =
        handle != 0L && loadedModelId == model.id && bridge.localLoaded(handle)

    /**
     * Open a handle for [model], replacing any other model's. BLOCKS on the weights.
     *
     * Returns "" when ready, otherwise why not. The two failures are worth telling
     * apart: no handle means llama.cpp was not built into this APK, while a load error
     * means the file is there and unreadable.
     */
    private suspend fun open(model: OnDeviceModelSpec): String = initLock.withLock {
        if (handle != 0L && loadedModelId == model.id) return@withLock ""
        if (handle != 0L) {
            bridge.localClose(handle)
            handle = 0L
            loadedModelId = null
        }
        val h = bridge.localOpen(models.fileFor(model).absolutePath, 0, THINKING)
        if (h == 0L) return@withLock "llama.cpp is not built into this app."
        val err = bridge.localLoad(h)
        if (err.isNotEmpty()) {
            bridge.localClose(h)
            return@withLock err
        }
        handle = h
        loadedModelId = model.id
        ""
    }

    /**
     * Warm the weights ahead of the first question — see [OnDeviceLlmEngine.preload].
     * Silent on failure: a model that cannot load will say so properly when a turn asks.
     */
    override suspend fun preload(model: OnDeviceModelSpec) {
        if (isLoaded(model) || !models.isReady(model)) return
        withContext(Dispatchers.Default) { runCatching { open(model) } }
    }

    override fun generate(history: List<ChatTurn>, model: OnDeviceModelSpec): Flow<ChatStreamEvent> =
        channelFlow {
            if (!models.isReady(model)) {
                send(ChatStreamEvent.Error("On-device model not downloaded yet."))
                send(ChatStreamEvent.End)
                return@channelFlow
            }
            // Announce the load BEFORE starting it: reading a few GB takes seconds, and
            // without this the turn looks like a model already writing.
            if (!isLoaded(model)) send(ChatStreamEvent.Loading)
            val err = open(model)
            if (err.isNotEmpty()) {
                send(ChatStreamEvent.Error(err))
                send(ChatStreamEvent.End)
                return@channelFlow
            }
            if (history.lastOrNull { it.fromUser }?.text.isNullOrBlank()) {
                send(ChatStreamEvent.End)
                return@channelFlow
            }

            // The whole transcript, every turn. C++ owns the window: it drops the oldest
            // messages until the prompt fits, keeping n_reply_reserve tokens free for the
            // answer. Trimming here as well would just make two policies disagree.
            val roles = Array(history.size) { if (history[it].fromUser) "user" else "assistant" }
            val contents = Array(history.size) { history[it].text }

            // BLOCKS this coroutine's thread for the whole reply, calling the sink inline
            // per token. trySend cannot fail — buffer(UNLIMITED) below fuses into this
            // flow's own channel — and returning false is what stops a decode loop that
            // a cancelled collector no longer wants.
            bridge.localGenerate(handle, roles, contents, SYSTEM_PROMPT) { type, text ->
                val event = when (type) {
                    EV_THINKING -> ChatStreamEvent.Thinking(text)
                    EV_ERROR -> ChatStreamEvent.Error(text)
                    else -> ChatStreamEvent.Reply(text)
                }
                trySend(event).isSuccess && isActive
            }
            // Cancellation closed the channel already; sending End would throw over it.
            if (isActive) send(ChatStreamEvent.End)
        }
            // Fuses into channelFlow's own capacity rather than adding a stage after it,
            // which is what lets the native callback use the non-suspending trySend: a
            // rendezvous channel would drop every token the collector was not ready for.
            .buffer(Channel.UNLIMITED)
            // Off the main thread, for the same reason the LiteRT engine says so at
            // length: a bare flow builder runs in the COLLECTOR's context, and that is
            // viewModelScope. The load blocks for seconds and every token blocks again.
            .flowOn(Dispatchers.Default)

    override fun close() {
        if (handle != 0L) {
            bridge.localCancel(handle)
            bridge.localClose(handle)
            handle = 0L
        }
        loadedModelId = null
    }

    private companion object {
        /// Mirrors the codes in src/platform/android_jni.cpp.
        const val EV_THINKING = 1
        const val EV_ERROR = 2

        /**
         * Reasoning off. Not a preference — llama_chat_apply_template has no Jinja
         * engine, so a model's `enable_thinking` conditional never fires and the default
         * is whatever its training happened to pick. See LocalOptions::thinking for why
         * off is the right constant at these sizes.
         */
        const val THINKING = 0

        const val SYSTEM_PROMPT = "You are Mirobody's private on-device health assistant. " +
            "Answer concisely. You have no internet or tools; rely only on the conversation."
    }
}

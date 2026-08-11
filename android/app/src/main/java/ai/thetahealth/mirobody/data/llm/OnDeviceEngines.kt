package ai.thetahealth.mirobody.data.llm

import ai.thetahealth.mirobody.data.chat.dto.ChatStreamEvent
import kotlinx.coroutines.flow.Flow

/**
 * The two on-device runtimes behind one [OnDeviceLlmEngine], picked per model.
 *
 * A model says which engine it needs ([OnDeviceModelSpec.runtime]) because the file
 * format is not a hint but the whole answer: LiteRT-LM reads `.litertlm` and llama.cpp
 * reads `.gguf`, and neither can be talked into the other. Nothing above this — chat,
 * the picker, the repository — knows there are two.
 *
 * Both stay alive at once. They hold no weights until something asks, and the
 * alternative (tear one down when the other is picked) would make switching models cost
 * a reload in each direction rather than one.
 */
class OnDeviceEngines(
    private val litert: OnDeviceLlmEngine,
    private val llama: OnDeviceLlmEngine,
) : OnDeviceLlmEngine {

    private fun engineFor(model: OnDeviceModelSpec): OnDeviceLlmEngine = when (model.runtime) {
        OnDeviceRuntime.LITERT_LM -> litert
        OnDeviceRuntime.LLAMA_CPP -> llama
    }

    override fun generate(history: List<ChatTurn>, model: OnDeviceModelSpec): Flow<ChatStreamEvent> =
        engineFor(model).generate(history, model)

    override suspend fun preload(model: OnDeviceModelSpec) = engineFor(model).preload(model)

    /** Closes both: this is app teardown, not a model switch. */
    override fun close() {
        litert.close()
        llama.close()
    }
}

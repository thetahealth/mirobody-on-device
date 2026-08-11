import Foundation

/// The two on-device runtimes behind one `OnDeviceLlmEngine`, picked per model.
/// Mirrors Android's `data/llm/OnDeviceEngines.kt`.
///
/// A model says which engine it needs (`OnDeviceModelSpec.runtime`) because the file
/// format is not a hint but the whole answer: LiteRT-LM reads `.litertlm` and llama.cpp
/// reads `.gguf`, and neither can be talked into the other. Nothing above this — chat,
/// the picker, the repository — knows there are two.
///
/// Both stay alive at once. They hold no weights until something asks, and the
/// alternative (tear one down when the other is picked) would make switching models cost
/// a reload in each direction rather than one.
final class OnDeviceEngines: OnDeviceLlmEngine {

    private let liteRt: OnDeviceLlmEngine
    private let llama: OnDeviceLlmEngine

    init(liteRt: OnDeviceLlmEngine, llama: OnDeviceLlmEngine) {
        self.liteRt = liteRt
        self.llama = llama
    }

    private func engine(for model: OnDeviceModelSpec) -> OnDeviceLlmEngine {
        switch model.runtime {
        case .liteRtLm: return liteRt
        case .llamaCpp: return llama
        }
    }

    func generate(history: [ChatTurn], model: OnDeviceModelSpec) -> AsyncStream<ChatStreamEvent> {
        engine(for: model).generate(history: history, model: model)
    }

    /// Closes both: this is app teardown, not a model switch.
    func close() {
        liteRt.close()
        llama.close()
    }
}

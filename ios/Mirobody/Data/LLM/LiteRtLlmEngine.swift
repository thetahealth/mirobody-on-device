import Foundation
#if canImport(LiteRTLM)
import LiteRTLM
#endif

/// On-device LLM via LiteRT-LM (any model in `OnDeviceModel.catalog`). Fully offline.
/// Mirrors Android's `data/llm/LiteRtLlmEngine.kt`.
///
/// Guarded by `#if canImport(LiteRTLM)`: until the LiteRT-LM Swift package is added
/// to the project (see `project.yml`), the app still builds and on-device turns
/// surface a friendly "not built in" error instead of failing to compile. Symbol
/// names track the LiteRT-LM Swift guide:
///   https://developers.google.com/edge/litert-lm/swift
final class LiteRtLlmEngine: OnDeviceLlmEngine {

    private let models: ModelManager

    init(models: ModelManager) {
        self.models = models
    }

#if canImport(LiteRTLM)
    private let state = EngineState()

    func generate(history: [ChatTurn], model: OnDeviceModelSpec) -> AsyncStream<ChatStreamEvent> {
        AsyncStream { continuation in
            let task = Task {
                guard models.isReady(model) else {
                    continuation.yield(.error(message: "On-device model not downloaded yet."))
                    continuation.yield(.end)
                    continuation.finish()
                    return
                }
                let question = history.last(where: { $0.fromUser })?.text ?? ""
                guard !question.isEmpty else {
                    continuation.yield(.end)
                    continuation.finish()
                    return
                }
                do {
                    let conversation = try await state.conversation(
                        modelId: model.id,
                        modelPath: models.fileURL(model).path,
                        history: history
                    )
                    // The SDK may emit cumulative or delta chunks; diff against what we've
                    // already emitted so the UI always receives pure deltas (the SSE contract).
                    var emitted = ""
                    for try await chunk in conversation.sendMessageStream(Message(question)) {
                        let full = chunk.toString
                        let delta = full.hasPrefix(emitted) ? String(full.dropFirst(emitted.count)) : full
                        if !delta.isEmpty {
                            emitted = full.hasPrefix(emitted) ? full : emitted + full
                            continuation.yield(.reply(delta: delta))
                        }
                    }
                    continuation.yield(.end)
                } catch {
                    continuation.yield(.error(message: error.localizedDescription))
                    continuation.yield(.end)
                }
                continuation.finish()
            }
            continuation.onTermination = { _ in task.cancel() }
        }
    }

    func close() {
        Task { await state.close() }
    }
#else
    func generate(history: [ChatTurn], model: OnDeviceModelSpec) -> AsyncStream<ChatStreamEvent> {
        AsyncStream { continuation in
            continuation.yield(.error(message: "On-device model support is not built into this app."))
            continuation.yield(.end)
            continuation.finish()
        }
    }

    func close() {}
#endif
}

#if canImport(LiteRTLM)
/// Serializes lazy Engine/Conversation creation. `Engine.initialize()` is expensive
/// (several seconds, ~3 GB RAM); the engine and a single multi-turn Conversation are
/// created once and reused. A turn whose history is just the new question (fresh chat)
/// resets the conversation.
private actor EngineState {
    private var engine: Engine?
    private var conversation: Conversation?
    /// Which model the live engine was loaded from; a different one forces a reload.
    private var loadedModelId: String?

    func conversation(modelId: String, modelPath: String, history: [ChatTurn]) async throws -> Conversation {
        let engine: Engine
        if let existing = self.engine, loadedModelId == modelId {
            engine = existing
        } else {
            // Different (or first) model: drop any live engine/conversation, load fresh.
            conversation = nil
            self.engine = nil
            let config = try EngineConfig(
                modelPath: modelPath,
                backend: .cpu(),
                maxNumTokens: 1024,
                cacheDir: NSTemporaryDirectory()
            )
            let created = Engine(engineConfig: config)
            try await created.initialize()
            self.engine = created
            loadedModelId = modelId
            engine = created
        }

        if history.count <= 1 { conversation = nil }
        if let existing = conversation { return existing }

        let sampler = try SamplerConfig(topK: 40, topP: 0.95, temperature: 0.8)
        // NB: don't seed the prior transcript into the system prompt — on small on-device
        // models the context window is tiny (e.g. Qwen3 0.6B is 2048 tokens) and a
        // restored/long history overflows it. Multi-turn within a session is carried by
        // the reused Conversation itself; the system prompt stays a short fixed instruction.
        let config = ConversationConfig(
            systemMessage: Message(Self.systemPrompt),
            samplerConfig: sampler
        )
        let created = try await engine.createConversation(with: config)
        conversation = created
        return created
    }

    func close() {
        conversation = nil
        engine = nil
        loadedModelId = nil
    }

    static let systemPrompt = "You are Mirobody's private on-device health assistant. " +
        "Answer concisely. You have no internet or tools; rely only on the conversation."
}
#endif

import Foundation
#if canImport(LiteRTLM)
import LiteRTLM
#endif

/// On-device LLM backed by Gemma 4 (E2B) via LiteRT-LM. Fully offline. Mirrors
/// Android's `data/llm/LiteRtLlmEngine.kt`.
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

    func generate(history: [ChatTurn]) -> AsyncStream<ChatStreamEvent> {
        AsyncStream { continuation in
            let task = Task {
                guard models.isReady else {
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
                        modelPath: ModelManager.modelURL.path,
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
    func generate(history: [ChatTurn]) -> AsyncStream<ChatStreamEvent> {
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

    func conversation(modelPath: String, history: [ChatTurn]) async throws -> Conversation {
        let engine: Engine
        if let existing = self.engine {
            engine = existing
        } else {
            let config = try EngineConfig(
                modelPath: modelPath,
                backend: .cpu(),
                maxNumTokens: 1024,
                cacheDir: NSTemporaryDirectory()
            )
            let created = Engine(engineConfig: config)
            try await created.initialize()
            self.engine = created
            engine = created
        }

        if history.count <= 1 { conversation = nil }
        if let existing = conversation { return existing }

        let sampler = try SamplerConfig(topK: 40, topP: 0.95, temperature: 0.8)
        let config = ConversationConfig(
            systemMessage: Message(Self.systemInstruction(prior: Array(history.dropLast()))),
            samplerConfig: sampler
        )
        let created = try await engine.createConversation(with: config)
        conversation = created
        return created
    }

    func close() {
        conversation = nil
        engine = nil
    }

    static func systemInstruction(prior: [ChatTurn]) -> String {
        let base = "You are Mirobody's private on-device health assistant. Answer concisely. " +
            "You have no internet or tools; rely only on the conversation."
        if prior.isEmpty { return base }
        let transcript = prior
            .map { ($0.fromUser ? "User: " : "Assistant: ") + $0.text }
            .joined(separator: "\n")
        return base + "\n\nConversation so far:\n" + transcript
    }
}
#endif

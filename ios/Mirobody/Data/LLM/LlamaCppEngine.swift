import Foundation

/// On-device LLM via llama.cpp, for the GGUF half of `OnDeviceModel.catalog`.
/// Mirrors Android's `data/llm/LlamaCppEngine.kt`.
///
/// The engine itself is C++ (`src/llm/local.cpp`, the same client Android, HarmonyOS
/// and Qt use); this is the Swift seam onto it, through the C API in `src/mirobody.h`.
/// Everything with an opinion — the context ladder, sampling, the `<think>` split, the
/// sliding history window — lives on that side, so all four frontends behave the same
/// and a fix lands once. What is left here is lifecycle: which model is loaded, and one
/// turn at a time.
///
/// Guarded by `#if MIROBODY_EMBEDDED`, exactly as `ServerController` is: the C symbols
/// are only linked once `mirobody.xcframework` is built and added (see `ios/README.md`),
/// and a pure-client build must still compile. Without it every turn reports that the
/// engine is not built in, which is the same graceful degradation the LiteRT lane gives
/// when its package is absent.
///
/// The native handle owns the weights for its life, because loading them is seconds and
/// a few GB; switching models tears it down and opens another. Unlike the LiteRT path
/// there is no conversation object — every turn replays the transcript and C++ trims it
/// to fit, which is why history is passed in full on each call.
final class LlamaCppEngine: OnDeviceLlmEngine {

    private let models: ModelManager

    init(models: ModelManager) {
        self.models = models
    }

    static let systemPrompt = "You are Mirobody's private on-device health assistant. " +
        "Answer concisely. You have no internet or tools; rely only on the conversation."

#if MIROBODY_EMBEDDED

    /// Serializes open/close against a turn. An actor rather than a lock because the
    /// only contention is a model switch racing a turn, and both are already async.
    private actor State {
        var handle: OpaquePointer?
        var loadedModelId: String?

        func adopt(_ new: OpaquePointer, id: String) {
            if let old = handle { mirobody_llm_close(old) }
            handle = new
            loadedModelId = id
        }

        func current(for id: String) -> OpaquePointer? {
            loadedModelId == id ? handle : nil
        }

        /// Whatever is open, regardless of which model — the only thing a cancel
        /// needs to know, since there is at most one engine and one turn.
        func live() -> OpaquePointer? { handle }

        func release() {
            if let h = handle {
                mirobody_llm_cancel(h)
                mirobody_llm_close(h)
            }
            handle = nil
            loadedModelId = nil
        }
    }

    private let state = State()

    /// Open a handle for `model`, replacing any other model's, and read the weights.
    /// BLOCKS for seconds — callers must already be off the main actor.
    ///
    /// Returns nil on success, otherwise why not. The two failures are worth telling
    /// apart: no handle means llama.cpp was not built into this binary, while a load
    /// error means the file is there and unreadable.
    private func engine(for model: OnDeviceModelSpec) async -> (OpaquePointer?, String?) {
        if let live = await state.current(for: model.id) { return (live, nil) }
        guard mirobody_llm_available() != 0 else {
            return (nil, "llama.cpp is not built into this app.")
        }
        let path = models.fileURL(model).path
        // 0 threads keeps the library's own tuned default, which is NOT
        // processorCount; 0 thinking matches what the model authors chose at these
        // sizes. See LocalOptions in src/llm/local.hpp for both.
        guard let handle = mirobody_llm_open(path, 0, 0) else {
            return (nil, "Could not start the on-device engine.")
        }
        if let err = mirobody_llm_load(handle), strlen(err) > 0 {
            let message = String(cString: err)
            mirobody_llm_close(handle)
            return (nil, message)
        }
        await state.adopt(handle, id: model.id)
        return (handle, nil)
    }

    func generate(history: [ChatTurn], model: OnDeviceModelSpec) -> AsyncStream<ChatStreamEvent> {
        AsyncStream { continuation in
            // Detached, and not merely async: mirobody_llm_generate blocks its thread
            // for the whole reply — the load reads a few GB and every token blocks
            // again — so running it on the cooperative pool's shared threads would
            // starve everything else scheduled there.
            let task = Task.detached(priority: .userInitiated) { [models] in
                guard models.isReady(model) else {
                    continuation.yield(.error(message: "On-device model not downloaded yet."))
                    continuation.yield(.end)
                    continuation.finish()
                    return
                }
                guard history.last(where: { $0.fromUser })?.text.isEmpty == false else {
                    continuation.yield(.end)
                    continuation.finish()
                    return
                }

                let (handle, error) = await self.engine(for: model)
                guard let handle else {
                    continuation.yield(.error(message: error ?? "On-device generation failed"))
                    continuation.yield(.end)
                    continuation.finish()
                    return
                }

                // The whole transcript, every turn. C++ owns the window: it drops the
                // oldest messages until the prompt fits, keeping a reserve free for the
                // answer. Trimming here as well would just make two policies disagree.
                let roles = history.map { $0.fromUser ? "user" : "assistant" }
                let contents = history.map { $0.text }

                // The C callback is a bare function pointer, so it can capture nothing;
                // the continuation travels as `user_data` inside this box.
                let sink = LlamaEventSink { continuation.yield($0) }

                withCStringArray(roles) { rolePtrs in
                    withCStringArray(contents) { contentPtrs in
                        _ = mirobody_llm_generate(
                            handle, rolePtrs, contentPtrs, Int32(history.count),
                            Self.systemPrompt,
                            { kind, text, userData in
                                guard let userData, let text else { return 0 }
                                let sink = Unmanaged<LlamaEventSink>.fromOpaque(userData)
                                    .takeUnretainedValue()
                                let s = String(cString: text)
                                switch kind {
                                case MIROBODY_LLM_THINKING: sink.yield(.thinking(delta: s))
                                case MIROBODY_LLM_ERROR:    sink.yield(.error(message: s))
                                default:                    sink.yield(.reply(delta: s))
                                }
                                // Non-zero keeps streaming. Cancellation is handled by
                                // onTermination below, which reaches the decode loop
                                // from another thread — this one is blocked inside it.
                                return 1
                            },
                            Unmanaged.passUnretained(sink).toOpaque())
                    }
                }

                continuation.yield(.end)
                continuation.finish()
            }

            continuation.onTermination = { _ in
                task.cancel()
                // The blocked thread cannot notice a cancelled Task, so tell the engine
                // directly: mirobody_llm_cancel is safe from another thread and stops
                // the decode loop at the next token. Without it, abandoning a reply
                // leaves a phone decoding for minutes in someone's pocket.
                Task.detached { await self.cancelInFlight() }
            }
        }
    }

    private func cancelInFlight() async {
        if let h = await state.live() { mirobody_llm_cancel(h) }
    }

    func close() {
        Task { await state.release() }
    }

#else

    func generate(history: [ChatTurn], model: OnDeviceModelSpec) -> AsyncStream<ChatStreamEvent> {
        AsyncStream { continuation in
            continuation.yield(.error(
                message: "The llama.cpp engine isn't built into this app."))
            continuation.yield(.end)
            continuation.finish()
        }
    }

    func close() {}

#endif
}

#if MIROBODY_EMBEDDED
/// Carries the stream continuation across the C boundary. A C function pointer captures
/// nothing, so the only way through is `user_data` — and that has to be a class, because
/// only a reference type has a stable address to pass as one.
private final class LlamaEventSink {
    let yield: (ChatStreamEvent) -> Void
    init(_ yield: @escaping (ChatStreamEvent) -> Void) { self.yield = yield }
}

/// Call `body` with `strings` as a C `const char* const*`, valid for the call only.
///
/// Hand-rolled because Swift has no public equivalent: `withCString` nests one string
/// at a time, and an array of them cannot be expressed that way without recursion. The
/// duplicates are freed on every path, including a throw out of `body`.
private func withCStringArray<R>(
    _ strings: [String],
    _ body: (UnsafePointer<UnsafePointer<CChar>?>?) -> R
) -> R {
    var pointers: [UnsafePointer<CChar>?] = strings.map { UnsafePointer(strdup($0)) }
    defer { for p in pointers { free(UnsafeMutableRawPointer(mutating: p)) } }
    return pointers.withUnsafeBufferPointer { body($0.baseAddress) }
}
#endif

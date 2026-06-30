import Foundation

/// One settled turn of a conversation, fed to the on-device model as context.
/// Mirrors Android's `data/llm/OnDeviceLlmEngine.kt` `ChatTurn`.
struct ChatTurn {
    let fromUser: Bool
    let text: String
}

/// A locally-hosted LLM that produces the same `ChatStreamEvent` stream the remote
/// `/api/chat` SSE path does, so the existing chat UI renders on-device replies with
/// no UI changes. Runs fully on-device — no network, no server.
///
/// The concrete implementation is `LiteRtLlmEngine` (Gemma 4 via LiteRT-LM). The
/// protocol is runtime-agnostic, matching the Android `OnDeviceLlmEngine` seam.
protocol OnDeviceLlmEngine: AnyObject {
    /// Stream a reply for `history`; the final entry is the new user turn. Emits
    /// `.reply` deltas (and optional `.thinking`), then `.end`. Failures surface as
    /// `.error` rather than throwing, mirroring the SSE client's contract.
    func generate(history: [ChatTurn]) -> AsyncStream<ChatStreamEvent>

    /// Release native resources (the model file stays on disk). Safe to call repeatedly.
    func close()
}

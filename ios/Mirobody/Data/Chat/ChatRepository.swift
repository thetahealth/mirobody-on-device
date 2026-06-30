import Foundation

/// Chat + history API. Mirrors `data/chat/ChatRepository.kt`.
///
/// Endpoints: `GET /api/providers`, `GET /api/history`, `POST /api/history/delete`,
/// and the SSE `POST /api/chat` (delegated to `ChatStreamClient`).
final class ChatRepository {
    private let api: ApiClient
    private let streamClient: ChatStreamClient
    private let onDeviceEngine: OnDeviceLlmEngine

    init(api: ApiClient, settings: SettingsStore, onDeviceEngine: OnDeviceLlmEngine) {
        self.api = api
        self.streamClient = ChatStreamClient(api: api)
        self.onDeviceEngine = onDeviceEngine
    }

    func listProviders() async throws -> [ProviderInfo] {
        try await api.get("/api/providers")
    }

    func history(page: Int = 0, pageSize: Int = 20) async throws -> [SessionSummary] {
        let resp: HistoryResponse = try await api.get("/api/history?page=\(page)&page_size=\(pageSize)")
        return resp.summaries
    }

    func deleteHistory(sessionId: String) async throws {
        try await api.postEnsureOk("/api/history/delete", HistoryDeleteRequest(sessionId: sessionId))
    }

    func chat(
        sessionId: String,
        question: String,
        agentCode: String,
        provider: String,
        language: String,
        subject: Int64 = 0,
        attachments: [ChatAttachment] = []
    ) -> AsyncStream<ChatStreamEvent> {
        streamClient.stream(
            ChatStreamRequest(
                question: question,
                sessionId: sessionId,
                agent: agentCode,
                provider: provider,
                language: language,
                subject: subject > 0 ? String(subject) : nil
            ),
            attachments: attachments
        )
    }

    /// Local, offline turn: routes to the on-device engine (Gemma 4) instead of the
    /// server. `history`'s final entry is the new user question. Emits the same
    /// `ChatStreamEvent` stream the SSE path does.
    func chatOnDevice(history: [ChatTurn]) -> AsyncStream<ChatStreamEvent> {
        onDeviceEngine.generate(history: history)
    }
}

import Foundation
import Combine

/// Drives the chat screen — mirrors `ui/chat/ChatViewModel.kt`.
@MainActor
final class ChatViewModel: ObservableObject {
    @Published private(set) var sessionId = UUID().uuidString
    @Published private(set) var messages: [ChatMessage] = []
    @Published var input = ""
    @Published private(set) var sending = false
    @Published private(set) var providers: [ProviderInfo] = []
    @Published private(set) var selected: ProviderInfo?
    @Published private(set) var error: String?
    @Published private(set) var language = "en"
    /// Files staged in the composer for the next turn (cleared on send).
    @Published private(set) var attachments: [ChatAttachment] = []
    /// State of the on-device model file, driving the on-device provider's UI.
    @Published private(set) var onDeviceStatus: OnDeviceModelStatus = .absent

    private let repo: ChatRepository
    private let settings: SettingsStore
    private let errorBus: ErrorBus
    private let modelManager: ModelManager
    private var streamTask: Task<Void, Never>?
    private var cancellables: Set<AnyCancellable> = []

    init(repo: ChatRepository, settings: SettingsStore, errorBus: ErrorBus, modelManager: ModelManager) {
        self.repo = repo
        self.settings = settings
        self.errorBus = errorBus
        self.modelManager = modelManager
        self.language = settings.language
        self.onDeviceStatus = modelManager.status
        settings.$language.sink { [weak self] lang in self?.language = lang }.store(in: &cancellables)
        modelManager.$status.sink { [weak self] st in self?.onDeviceStatus = st }.store(in: &cancellables)
        loadProviders()
    }

    /// Start (or resume) the on-device model download.
    func downloadOnDeviceModel() { modelManager.startDownload() }
    /// Pause the in-flight download (resumable).
    func pauseOnDeviceModel() { modelManager.pauseDownload() }
    /// Remove the on-device model to reclaim storage.
    func deleteOnDeviceModel() { modelManager.delete() }

    func loadProviders() {
        Task {
            let savedName = settings.selectedProviderName
            do {
                // Always append the on-device provider alongside the server's.
                let list = try await repo.listProviders() + [ProviderInfo.onDevice]
                providers = list
                if selected == nil { selected = list.first { $0.key == savedName } ?? list.first }
                error = nil
            } catch {
                errorBus.emit(error)
                self.error = localizedMessage(error.toAppError(), language: language)
                // The server is unreachable, but on-device chat still works offline —
                // keep it available rather than leaving the picker empty.
                let list = [ProviderInfo.onDevice]
                providers = list
                if selected == nil { selected = list.first { $0.key == savedName } ?? list.first }
            }
        }
    }

    func onProviderSelected(_ provider: ProviderInfo) {
        selected = provider
        settings.setSelectedProviderName(provider.key)
    }

    func addAttachment(_ attachment: ChatAttachment) {
        attachments.append(attachment)
    }

    func removeAttachment(_ id: ChatAttachment.ID) {
        attachments.removeAll { $0.id == id }
    }

    func send() {
        let question = input.trimmingCharacters(in: .whitespaces)
        let turnAttachments = attachments
        // Allow an attachment-only turn (no text), matching the web composer.
        guard (!question.isEmpty || !turnAttachments.isEmpty), !sending, let selected else { return }

        let userMsg = ChatMessage(
            id: "u-\(UUID().uuidString)", role: .user, text: question,
            attachmentNames: turnAttachments.map { $0.fileName }
        )
        let assistantId = "a-\(UUID().uuidString)"
        let assistantMsg = ChatMessage(id: assistantId, role: .assistant, streaming: true)
        messages.append(userMsg)
        messages.append(assistantMsg)
        input = ""
        attachments = []   // consumed by this turn
        sending = true
        error = nil

        streamTask?.cancel()
        streamTask = Task {
            let stream: AsyncStream<ChatStreamEvent>
            if selected.isOnDevice {
                // Offline path: hand the settled transcript (incl. this new question,
                // excluding the in-flight placeholder) to the on-device engine.
                let turns = messages
                    .filter { $0.id != assistantId && !$0.text.isEmpty }
                    .map { ChatTurn(fromUser: $0.role == .user, text: $0.text) }
                stream = repo.chatOnDevice(history: turns)
            } else {
                stream = repo.chat(
                    sessionId: sessionId,
                    question: question,
                    agent: selected.agent,
                    provider: selected.provider,
                    language: language,
                    attachments: turnAttachments
                )
            }
            for await event in stream {
                applyEvent(targetId: assistantId, event: event)
            }
            sending = false
        }
    }

    private func applyEvent(targetId: String, event: ChatStreamEvent) {
        switch event {
        case .reply(let delta):
            updateMessage(targetId) { $0.text += delta }
        case .thinking(let delta):
            updateMessage(targetId) { $0.thinking += delta }
        case .queryTitle(let toolId, let title):
            updateMessage(targetId) { upsertTool(&$0, toolId) { $0.title += title } }
        case .queryArguments(let toolId, let args):
            updateMessage(targetId) { upsertTool(&$0, toolId) { $0.argumentsJson += args } }
        case .queryDetail(let toolId, let detail):
            updateMessage(targetId) { upsertTool(&$0, toolId) { $0.resultJson += detail; $0.resultReceived = true } }
        case .image(let url):
            updateMessage(targetId) { $0.imageUrls.append(url) }
        case .chart(let optionJson):
            updateMessage(targetId) { $0.chartOptions.append(optionJson) }
        case .error(let message):
            // Surfaced inline on the message bubble (matches Kotlin applyEvent, which
            // does not route stream errors through the global ErrorBus).
            updateMessage(targetId) { $0.streaming = false; $0.error = message }
        case .end:
            updateMessage(targetId) { $0.streaming = false }
        case .stats(let stats):
            updateMessage(targetId) { $0.costStats = stats }
        case .heartbeat, .id, .unknown:
            break
        }
    }

    private func updateMessage(_ id: String, _ transform: (inout ChatMessage) -> Void) {
        guard let idx = messages.firstIndex(where: { $0.id == id }) else { return }
        transform(&messages[idx])
    }

    /// Append a transform to the tool keyed by `toolId`, creating it if unseen.
    private func upsertTool(_ msg: inout ChatMessage, _ toolId: String, _ transform: (inout ToolCall) -> Void) {
        if let idx = msg.toolCalls.firstIndex(where: { $0.id == toolId }) {
            transform(&msg.toolCalls[idx])
        } else {
            var tc = ToolCall(id: toolId)
            transform(&tc)
            msg.toolCalls.append(tc)
        }
    }

    deinit { streamTask?.cancel() }
}

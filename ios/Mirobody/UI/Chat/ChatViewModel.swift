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
    /// Per-model download/ready state, keyed by OnDeviceModelSpec.id — drives the model
    /// manager and which on-device models appear in the picker.
    @Published private(set) var onDeviceStatuses: [String: OnDeviceModelStatus] = [:]
    /// Privacy mode: entering swaps the session for a fresh ephemeral one; while on,
    /// turns aren't persisted and each request carries `incognito:true`. Mirrors web.
    @Published private(set) var incognito = false
    /// The server-side thread id for the current conversation, learned from the
    /// `conversation` SSE event or on resume. Empty until the first turn lands.
    @Published private(set) var conversationId = ""
    /// A conversation shared TO the user opens read-only: the composer is hidden.
    @Published private(set) var readOnly = false

    private let repo: ChatRepository
    private let settings: SettingsStore
    private let errorBus: ErrorBus
    private let modelManager: ModelManager
    private var streamTask: Task<Void, Never>?
    private var cancellables: Set<AnyCancellable> = []
    /// Session stashed when entering incognito, restored on exit (messages + thread id).
    private var incognitoSaved: (messages: [ChatMessage], conversationId: String)?
    /// Server providers, kept so the picker can be rebuilt when downloaded models change;
    /// and the persisted selection key for restore.
    private var remoteProviders: [ProviderInfo] = []
    private var savedProviderKey: String?

    init(repo: ChatRepository, settings: SettingsStore, errorBus: ErrorBus, modelManager: ModelManager) {
        self.repo = repo
        self.settings = settings
        self.errorBus = errorBus
        self.modelManager = modelManager
        self.language = settings.language
        self.onDeviceStatuses = modelManager.statuses
        settings.$language.sink { [weak self] lang in self?.language = lang }.store(in: &cancellables)
        modelManager.$statuses.sink { [weak self] st in
            self?.onDeviceStatuses = st
            self?.rebuildProviders(statuses: st)
        }.store(in: &cancellables)
        loadProviders()
    }

    /// Start (or resume) a model download.
    func downloadOnDeviceModel(_ spec: OnDeviceModelSpec) { modelManager.startDownload(spec) }
    /// Pause an in-flight download (resumable).
    func pauseOnDeviceModel(_ spec: OnDeviceModelSpec) { modelManager.pauseDownload(spec) }
    /// Remove a downloaded model to reclaim storage.
    func deleteOnDeviceModel(_ spec: OnDeviceModelSpec) { modelManager.delete(spec) }

    func loadProviders() {
        Task {
            savedProviderKey = settings.selectedProviderName
            do {
                remoteProviders = try await repo.listProviders()
                error = nil
                rebuildProviders()
            } catch {
                errorBus.emit(error)
                self.error = localizedMessage(error.toAppError(), language: language)
                // The server is unreachable, but on-device chat still works offline —
                // keep the picker populated (manage entry + any downloaded models).
                remoteProviders = []
                rebuildProviders()
            }
        }
    }

    /// Rebuild the provider picker: server providers + one entry per *downloaded*
    /// on-device model + the "manage on-device AI" entry (mirroring the Qt desktop
    /// client — undownloaded models aren't listed; the manage entry downloads them).
    /// Keeps the current selection if still present, else the saved one, else the first.
    private func rebuildProviders(statuses: [String: OnDeviceModelStatus]? = nil) {
        let st = statuses ?? onDeviceStatuses
        let downloaded = OnDeviceModel.catalog
            .filter { st[$0.id]?.isReady == true }
            .map { ProviderInfo.forModel($0) }
        let list = remoteProviders + downloaded + [ProviderInfo.manage]
        providers = list
        selected = selected.flatMap { s in list.first { $0.key == s.key } }
            ?? list.first { $0.key == savedProviderKey }
            ?? list.first
    }

    func onProviderSelected(_ provider: ProviderInfo) {
        selected = provider
        settings.setSelectedProviderName(provider.key)
    }

    /// Toggle privacy mode, swapping the whole session (mirrors the web client).
    /// Entering stashes the current thread and starts a fresh ephemeral one; leaving
    /// restores it. Inert while a reply is streaming.
    func toggleIncognito() {
        guard !sending else { return }
        streamTask?.cancel()
        if !incognito {
            incognitoSaved = (messages, conversationId)
            messages = []
            conversationId = ""
            readOnly = false
            incognito = true
        } else {
            let saved = incognitoSaved ?? ([], "")
            incognitoSaved = nil
            messages = saved.messages
            conversationId = saved.conversationId
            incognito = false
        }
    }

    /// Clear the current thread and start a new one (the drawer's "New chat").
    /// Leaves incognito state as-is (that's the toggle's job).
    func newChat() {
        guard !sending else { return }
        streamTask?.cancel()
        messages = []
        conversationId = ""
        readOnly = false
        sessionId = UUID().uuidString
        input = ""
    }

    /// Open a saved conversation from history and continue it: adopt its session id
    /// as the thread key so the next turn appends to it. A thread shared TO the user
    /// (not owned) opens read-only. Leaves incognito. Mirrors Android's
    /// `openConversation`.
    func openConversation(sessionId: String) {
        guard !sending else { return }
        streamTask?.cancel()
        incognitoSaved = nil
        Task {
            do {
                let detail = try await repo.conversation(sessionId: sessionId)
                var msgs: [ChatMessage] = []
                for m in detail.messages where m.role == "user" || m.role == "assistant" {
                    msgs.append(ChatMessage(
                        id: "h-\(sessionId)-\(msgs.count)",
                        role: m.role == "assistant" ? .assistant : .user,
                        text: m.content
                    ))
                }
                messages = msgs
                self.sessionId = sessionId
                conversationId = detail.id.nonBlank ?? sessionId
                readOnly = !detail.owned
                incognito = false
                input = ""
            } catch {
                errorBus.emit(error)
            }
        }
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
        // The "manage on-device AI" entry isn't a chat provider; selecting it opens the
        // manager (handled in the view), never sends.
        if selected.isManageEntry { return }

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
            if let onDeviceSpec = selected.modelSpec {
                // Offline path: hand the settled transcript (incl. this new question,
                // excluding the in-flight placeholder) to the on-device engine.
                let turns = messages
                    .filter { $0.id != assistantId && !$0.text.isEmpty }
                    .map { ChatTurn(fromUser: $0.role == .user, text: $0.text) }
                stream = repo.chatOnDevice(history: turns, model: onDeviceSpec)
            } else {
                stream = repo.chat(
                    sessionId: sessionId,
                    question: question,
                    agent: selected.agent,
                    provider: selected.provider,
                    language: language,
                    attachments: turnAttachments,
                    incognito: incognito
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
        case .conversation(let id):
            if conversationId != id { conversationId = id }
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

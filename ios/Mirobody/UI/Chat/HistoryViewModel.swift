import Foundation

/// Drives the history screen — mirrors `ui/chat/HistoryViewModel.kt`.
@MainActor
final class HistoryViewModel: ObservableObject {
    @Published private(set) var items: [SessionSummary] = []
    @Published private(set) var loading = false
    @Published private(set) var error: String?

    private let repo: ChatRepository
    private let errorBus: ErrorBus
    private let language: String

    init(repo: ChatRepository, errorBus: ErrorBus, language: String) {
        self.repo = repo
        self.errorBus = errorBus
        self.language = language
    }

    func refresh() {
        loading = true
        error = nil
        Task {
            do {
                items = try await repo.history()
                loading = false
            } catch {
                errorBus.emit(error)
                loading = false
                self.error = localizedMessage(error.toAppError(), language: language)
            }
        }
    }

    func deleteHistory(sessionId: String) {
        guard !sessionId.isEmpty else { return }
        // Optimistically drop the row; restore if the server call fails. The failure
        // is surfaced via the app-wide toast (ErrorBus).
        let previous = items
        items.removeAll { $0.sessionId == sessionId }
        Task {
            do {
                try await repo.deleteHistory(sessionId: sessionId)
            } catch {
                errorBus.emit(error)
                items = previous
            }
        }
    }
}

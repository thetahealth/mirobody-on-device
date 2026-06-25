import Combine
import Foundation

/// App-wide error stream — mirrors `data/net/ErrorBus.kt`. View models emit; a
/// single collector at the root (`RootView`) renders a transient toast. The
/// subject drops events with no subscriber, so a slow/absent consumer never
/// blocks the emitter.
final class ErrorBus: ObservableObject {
    let errors = PassthroughSubject<AppError, Never>()

    func emit(_ error: AppError) {
        errors.send(error)
    }

    func emit(_ throwable: Error) {
        errors.send(throwable.toAppError())
    }
}

import SwiftUI

private enum AuthRoute: Hashable {
    case verify(email: String)
}

/// Top-level navigation + the runtime language / font-size / error-toast plumbing —
/// the iOS analogue of `MirobodyNavGraph.kt` plus the root Snackbar host in
/// `MainActivity`.
///
/// Routing is token-driven: when a token is present we show the chat screen, when it
/// is absent we show the auth flow. Because the token is `@Published`, a mid-session
/// 401 (which clears it) automatically pops back to login, and a successful sign-in
/// automatically advances to chat — no manual navigation needed.
struct RootView: View {
    @EnvironmentObject private var container: AppContainer
    @EnvironmentObject private var settings: SettingsStore
    @EnvironmentObject private var errorBus: ErrorBus

    @State private var authPath: [AuthRoute] = []
    @State private var toast: String?
    @State private var toastTask: Task<Void, Never>?

    var body: some View {
        content
            .environment(\.mbLanguage, settings.language)
            .environment(\.mbFontScale, fontScale(forOffset: settings.fontSizeOffset))
            .environment(\.locale, Locale(identifier: settings.language))
            // Mirror the whole UI for Arabic / Hebrew. The in-app language
            // override bypasses the system locale, so set the direction explicitly.
            .environment(\.layoutDirection, isRTL(settings.language) ? .rightToLeft : .leftToRight)
            .overlay(alignment: .bottom) { toastView }
            .onReceive(errorBus.errors) { error in
                showToast(localizedMessage(error, language: settings.language))
            }
    }

    @ViewBuilder
    private var content: some View {
        if settings.accessToken != nil {
            ChatView(container: container)
        } else {
            NavigationStack(path: $authPath) {
                EmailView(
                    container: container,
                    onCodeSent: { email in authPath.append(.verify(email: email)) },
                    onSignedIn: {}   // token lands in settings → content swaps to ChatView
                )
                .navigationDestination(for: AuthRoute.self) { route in
                    switch route {
                    case .verify(let email):
                        VerifyView(container: container, email: email)
                    }
                }
            }
        }
    }

    @ViewBuilder
    private var toastView: some View {
        if let toast {
            Text(toast)
                .mbFont(.bodyMedium)
                .foregroundColor(.white)
                .padding(.horizontal, 16).padding(.vertical, 12)
                .background(Color.black.opacity(0.85))
                .clipShape(RoundedRectangle(cornerRadius: 10))
                .padding(.bottom, 32).padding(.horizontal, 24)
                .transition(.opacity)
        }
    }

    private func showToast(_ message: String) {
        withAnimation { toast = message }
        toastTask?.cancel()
        toastTask = Task {
            try? await Task.sleep(nanoseconds: 3_000_000_000)
            if Task.isCancelled { return }
            withAnimation { toast = nil }
        }
    }
}

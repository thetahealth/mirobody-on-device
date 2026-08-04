import SwiftUI

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

    @State private var showSettingsDrawer = false
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
        // Token-driven routing. "Add account" (addingAccount) forces the login view
        // over a still-signed-in session so a second account can sign in; the
        // existing account's token stays stored (see SettingsStore.setAccessToken).
        if settings.accessToken != nil && !settings.addingAccount {
            // `.id` gives the chat a stable identity per account, so switching or
            // signing out to another account recreates it with fresh state — the
            // iOS analogue of Android's `onRelaunch`.
            ChatView(container: container)
                .id(settings.currentAccountId)
        } else {
            ZStack {
                // One screen, so the stack carries no path -- it is here for the
                // navigation bar that hosts the leading affordance below. Email and
                // one-time code live on the same card (EmailView), as they do on the web
                // and on Android; there is no verify screen to push.
                NavigationStack {
                    EmailView(
                        container: container,
                        onSignedIn: {}   // token lands in settings → content swaps to ChatView
                    )
                    // The auth flow's one leading affordance, deciding by the same rule
                    // as the web client's `leftMode`. Adding a second account: a Cancel
                    // backs out to the account still signed in — a drawer would be a
                    // dead end there. Otherwise: the hamburger, which is how these
                    // screens reach language / font size / backend now that the gear is
                    // gone. It sits here rather than on EmailView so the drawer and the
                    // Cancel stay one decision instead of two views' worth.
                    .toolbar {
                        ToolbarItem(placement: .navigationBarLeading) {
                            if settings.addingAccount && settings.accessToken != nil {
                                Button(L("common_cancel", settings.language)) {
                                    settings.addingAccount = false
                                }
                            } else {
                                DrawerMenuButton { showSettingsDrawer = true }
                            }
                        }
                    }
                }
                settingsDrawerOverlay
            }
            .animation(.easeInOut(duration: 0.25), value: showSettingsDrawer)
            // Signing in swaps this branch out without resetting the flag (RootView
            // itself is never recreated), so a later sign-out would come back with the
            // drawer already open. Start every visit to the auth flow closed.
            .onAppear { showSettingsDrawer = false }
        }
    }

    /// The auth screens' drawer — the app-settings group and nothing else. Same scrim +
    /// left-anchored panel as the chat drawer, so the two open identically.
    @ViewBuilder
    private var settingsDrawerOverlay: some View {
        if showSettingsDrawer {
            Color.black.opacity(0.4)
                .ignoresSafeArea()
                .onTapGesture { showSettingsDrawer = false }
                .transition(.opacity)
            HStack(spacing: 0) {
                SettingsDrawer(onDismiss: { showSettingsDrawer = false })
                    .frame(maxWidth: drawerMaxWidth)
                    .frame(width: mbDrawerWidth)
                    .ignoresSafeArea(edges: .bottom)
                Spacer(minLength: 0)
            }
            .transition(.move(edge: .leading))
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

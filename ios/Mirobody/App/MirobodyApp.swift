import SwiftUI
import FirebaseCore
import FirebaseAuth

/// App entry point — the iOS analogue of Android's `MirobodyApp` + `MainActivity`.
///
/// Owns the composition root (`AppContainer`) and the embedded-server controller,
/// injects them into the SwiftUI environment, and starts the in-process server on
/// launch (a no-op in pure-client builds). `RootView` then drives navigation and
/// applies the runtime language + font-size overrides.
@main
struct MirobodyApp: App {
    @StateObject private var container: AppContainer
    @StateObject private var server = ServerController()

    init() {
        _container = StateObject(wrappedValue: AppContainer())
    }

    var body: some Scene {
        WindowGroup {
            RootView()
                .environmentObject(container)
                .environmentObject(container.settings)
                .environmentObject(container.errorBus)
                .environmentObject(server)
                .onAppear { server.start() }
                // Forward the Google sign-in OAuth redirect to Firebase. Guarded so it
                // never touches Firebase in a build where it isn't configured.
                .onOpenURL { url in
                    if FirebaseApp.app() != nil, Auth.auth().canHandle(url) { return }
                    // WeChat sign-in returns via the app's URL scheme / universal link;
                    // hand it to the SDK (a no-op when the SDK isn't bundled).
                    _ = container.wechatAuthRepository.handleOpenURL(url)
                }
        }
    }
}

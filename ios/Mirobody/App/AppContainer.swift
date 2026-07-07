import Foundation

/// Composition root — the iOS analogue of Android's `di/AppContainer.kt`.
///
/// Owns the long-lived singletons (settings, error bus, API client, repositories)
/// and wires them together. Injected into the SwiftUI environment at the app root
/// so any view can reach a repository without prop-drilling.
@MainActor
final class AppContainer: ObservableObject {

    let settings: SettingsStore
    let errorBus: ErrorBus
    let apiClient: ApiClient

    let authRepository: AuthRepository
    let googleAuthRepository: GoogleAuthRepository
    let appleAuthRepository: AppleAuthRepository
    let wechatAuthRepository: WeChatAuthRepository
    let githubAuthRepository: GitHubAuthRepository
    let xAuthRepository: XAuthRepository
    let chatRepository: ChatRepository
    let serverConfigStore: ServerConfigStore
    let healthRepository: HealthKitRepository
    /// Direct BLE GATT sensor ingestion (HR strap / BP cuff / thermometer → FHIR),
    /// the fallback for standard medical sensors with no companion app. A singleton so
    /// a live connection survives sheet dismissal; posts through the same /fhir path.
    let bleHealthController: BleHealthController
    /// On-device private LLM (Gemma 4 via LiteRT-LM). The model file is downloaded on
    /// demand by `modelManager`; the engine loads it lazily on the first local turn.
    let modelManager: ModelManager

    init() {
        let settings = SettingsStore()
        let errorBus = ErrorBus()
        // The API client reads base URL + token from settings on every request and
        // clears the token on 401 (mirrors the OkHttp interceptor chain on Android).
        let apiClient = ApiClient(settings: settings)

        self.settings = settings
        self.errorBus = errorBus
        self.apiClient = apiClient
        self.authRepository = AuthRepository(api: apiClient, settings: settings)
        self.googleAuthRepository = GoogleAuthRepository(api: apiClient, settings: settings)
        self.appleAuthRepository = AppleAuthRepository(api: apiClient, settings: settings)
        self.wechatAuthRepository = WeChatAuthRepository(api: apiClient, settings: settings)
        self.githubAuthRepository = GitHubAuthRepository(api: apiClient, settings: settings)
        self.xAuthRepository = XAuthRepository(api: apiClient, settings: settings)
        let modelManager = ModelManager()
        self.modelManager = modelManager
        self.chatRepository = ChatRepository(
            api: apiClient,
            settings: settings,
            onDeviceEngine: LiteRtLlmEngine(models: modelManager)
        )
        self.serverConfigStore = ServerConfigStore(api: apiClient, settings: settings)
        // On-device Apple Health ingestion: reads HealthKit and POSTs FHIR
        // Observations to the embedded server's /fhir endpoint.
        self.healthRepository = HealthKitRepository(api: apiClient, settings: settings)
        // Direct BLE GATT sensor ingestion; the CBCentralManager is created lazily on
        // first scan, so no Bluetooth prompt fires at launch.
        self.bleHealthController = BleHealthController(api: apiClient)

        // Initialize Firebase from the bundled config, if the iOS values are filled in.
        // Email login works regardless; only Google sign-in depends on this.
        FirebaseInitializer.ensureInitialized()
    }
}

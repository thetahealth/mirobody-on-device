package ai.thetahealth.mirobody.di

import ai.thetahealth.mirobody.data.auth.AppleAuthRepository
import ai.thetahealth.mirobody.data.auth.AuthApi
import ai.thetahealth.mirobody.data.auth.AuthRepository
import ai.thetahealth.mirobody.data.auth.FirebaseInitializer
import ai.thetahealth.mirobody.data.auth.GithubAuthRepository
import ai.thetahealth.mirobody.data.auth.GoogleAuthRepository
import ai.thetahealth.mirobody.data.auth.WechatAuthRepository
import ai.thetahealth.mirobody.data.auth.XAuthRepository
import ai.thetahealth.mirobody.data.chat.ChatApi
import ai.thetahealth.mirobody.data.chat.ChatHistoryStore
import ai.thetahealth.mirobody.data.chat.ChatRepository
import ai.thetahealth.mirobody.data.chat.ChatStreamClient
import ai.thetahealth.mirobody.data.circle.CircleApi
import ai.thetahealth.mirobody.data.circle.CircleRepository
import ai.thetahealth.mirobody.data.config.ServerConfigApi
import ai.thetahealth.mirobody.data.config.ServerConfigStore
import ai.thetahealth.mirobody.data.health.EhrApi
import ai.thetahealth.mirobody.data.health.EhrRepository
import ai.thetahealth.mirobody.data.health.HealthApi
import ai.thetahealth.mirobody.data.health.HealthRepository
import ai.thetahealth.mirobody.data.health.HealthSourceFactory
import ai.thetahealth.mirobody.data.health.ble.BleHealthController
import ai.thetahealth.mirobody.data.health.hdp.HdpHealthController
import ai.thetahealth.mirobody.data.llm.LiteRtLlmEngine
import ai.thetahealth.mirobody.data.llm.LlamaCppEngine
import ai.thetahealth.mirobody.data.llm.MlKitTextService
import ai.thetahealth.mirobody.data.llm.OnDeviceEngines
import ai.thetahealth.mirobody.data.llm.OnDeviceLlmEngine
import ai.thetahealth.mirobody.data.llm.ModelManager
import ai.thetahealth.mirobody.data.net.AuthInterceptor
import ai.thetahealth.mirobody.data.net.BaseUrlInterceptor
import ai.thetahealth.mirobody.data.net.ErrorBus
import ai.thetahealth.mirobody.data.net.NetworkFactory
import ai.thetahealth.mirobody.data.net.UnauthorizedInterceptor
import ai.thetahealth.mirobody.data.settings.SettingsStore
import ai.thetahealth.mirobody.data.vendor.VendorApi
import ai.thetahealth.mirobody.data.vendor.VendorRepository
import android.content.Context
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.SupervisorJob
import kotlinx.coroutines.flow.SharingStarted
import kotlinx.coroutines.flow.stateIn
import retrofit2.create

class AppContainer(context: Context) {

    private val appContext: Context = context.applicationContext

    init {
        // Initialize the default FirebaseApp from assets/google-services.json before any
        // FirebaseAuth.getInstance() call (currently only used by GoogleAuthRepository).
        FirebaseInitializer.ensureInitialized(appContext)
    }

    val applicationScope: CoroutineScope =
        CoroutineScope(SupervisorJob() + Dispatchers.Default)

    val settings: SettingsStore = SettingsStore(appContext)

    val errorBus: ErrorBus = ErrorBus()

    private val baseUrlFlow = settings.baseUrl.stateIn(
        scope = applicationScope,
        started = SharingStarted.Eagerly,
        initialValue = SettingsStore.DEFAULT_BASE_URL,
    )

    val tokenFlow = settings.accessToken.stateIn(
        scope = applicationScope,
        started = SharingStarted.Eagerly,
        initialValue = null,
    )

    private val okHttp = NetworkFactory.okHttp(
        baseUrlInterceptor = BaseUrlInterceptor(baseUrlFlow),
        authInterceptor = AuthInterceptor(tokenFlow),
        unauthorizedInterceptor = UnauthorizedInterceptor(settings, applicationScope),
    )

    private val retrofit = NetworkFactory.retrofit(okHttp)

    private val authApi: AuthApi = retrofit.create()
    private val chatApi: ChatApi = retrofit.create()
    private val circleApi: CircleApi = retrofit.create()
    private val serverConfigApi: ServerConfigApi = retrofit.create()
    private val healthApi: HealthApi = retrofit.create()
    private val vendorApi: VendorApi = retrofit.create()
    private val ehrApi: EhrApi = retrofit.create()

    val authRepository: AuthRepository = AuthRepository(authApi, settings)

    // Vendor account management (Fitbit / Oura / Garmin / platforms) via /vendors/*.
    val vendorRepository: VendorRepository = VendorRepository(vendorApi)

    // EHR connect (SMART on FHIR) via /health/ehr/*.
    val ehrRepository: EhrRepository = EhrRepository(ehrApi)

    // On-device private LLM. Model files are downloaded on demand by ModelManager; an
    // engine loads one lazily, on the first local turn or when the user picks it.
    val modelManager: ModelManager = ModelManager(appContext)

    // Two runtimes, chosen per model rather than per app: LiteRT-LM runs the `.litertlm`
    // builds Google publishes for Gemma's E-series, llama.cpp runs any GGUF — which is
    // the only lane the current Qwen generation exists in. Neither wins outright; see
    // docs/on-device-llm.md for the measurements that settled it.
    private val onDeviceEngine: OnDeviceLlmEngine = OnDeviceEngines(
        litert = LiteRtLlmEngine(modelManager),
        llama = LlamaCppEngine(modelManager),
    )

    // Layered on-device GenAI (Gemini Nano via ML Kit) for bounded tasks like rewriting
    // a draft. Available only on AICore-capable devices; the UI hides it otherwise.
    val mlKitTextService: MlKitTextService = MlKitTextService(appContext)

    val chatRepository: ChatRepository = ChatRepository(
        api = chatApi,
        streamClient = ChatStreamClient(okHttp, NetworkFactory.json),
        onDeviceEngine = onDeviceEngine,
    )

    val chatHistoryStore: ChatHistoryStore = ChatHistoryStore(appContext, settings, NetworkFactory.json)

    val circleRepository: CircleRepository = CircleRepository(circleApi)

    val serverConfigStore: ServerConfigStore = ServerConfigStore(
        api = serverConfigApi,
        settings = settings,
        json = NetworkFactory.json,
        scope = applicationScope,
    )

    val googleAuthRepository: GoogleAuthRepository = GoogleAuthRepository(
        api = authApi,
        settings = settings,
    )

    val appleAuthRepository: AppleAuthRepository = AppleAuthRepository(
        api = authApi,
        settings = settings,
    )

    val wechatAuthRepository: WechatAuthRepository = WechatAuthRepository(
        context = appContext,
        api = authApi,
        settings = settings,
    )

    val githubAuthRepository: GithubAuthRepository = GithubAuthRepository(
        api = authApi,
        settings = settings,
    )

    val xAuthRepository: XAuthRepository = XAuthRepository(
        api = authApi,
        settings = settings,
    )

    // On-device health ingestion: reads Health Connect (GMS) or HMS Health Kit
    // (Huawei) and POSTs FHIR Observations to the embedded server's /fhir endpoint.
    val healthRepository: HealthRepository = HealthRepository(
        api = healthApi,
        sourceFactory = HealthSourceFactory(appContext),
    )

    // Direct BLE GATT sensor ingestion (HR strap / BP cuff / thermometer → FHIR),
    // the fallback for standard medical sensors with no companion app. A singleton so
    // a live connection survives dialog recomposition; posts through the same /fhir API.
    val bleHealthController: BleHealthController = BleHealthController(
        context = appContext,
        api = healthApi,
        scope = applicationScope,
    )

    // Legacy classic-Bluetooth HDP (IEEE 11073) ingestion — self-disables above API 28
    // (Android 9). Kept so old medical hardware can still reach the FHIR store.
    val hdpHealthController: HdpHealthController = HdpHealthController(
        context = appContext,
        api = healthApi,
        scope = applicationScope,
    )
}
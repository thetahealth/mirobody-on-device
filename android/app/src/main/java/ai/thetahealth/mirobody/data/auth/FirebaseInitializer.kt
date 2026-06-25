package ai.thetahealth.mirobody.data.auth

import android.content.Context
import com.google.firebase.FirebaseApp
import com.google.firebase.FirebaseOptions
import kotlinx.serialization.SerialName
import kotlinx.serialization.Serializable
import kotlinx.serialization.json.Json

/**
 * Initializes the default [FirebaseApp] from the bundled `assets/google-services.json`.
 *
 * The conventional setup applies the `com.google.gms.google-services` Gradle plugin,
 * which generates string resources at build time and lets Firebase's
 * `FirebaseInitProvider` auto-initialize the default app. We deliberately skip the
 * plugin and parse the JSON at runtime so initialization is explicit and lives
 * alongside the rest of the auth wiring.
 *
 * Call [ensureInitialized] once before any [com.google.firebase.auth.FirebaseAuth]
 * access — typically from the DI container constructor.
 */
object FirebaseInitializer {

    private val json = Json { ignoreUnknownKeys = true }

    @Synchronized
    fun ensureInitialized(context: Context) {
        val appContext = context.applicationContext
        if (FirebaseApp.getApps(appContext).isNotEmpty()) return

        val raw = appContext.assets.open(ASSET_NAME).bufferedReader().use { it.readText() }
        val cfg = json.decodeFromString<GoogleServicesJson>(raw)

        val client = cfg.client.firstOrNull()
            ?: error("$ASSET_NAME has no client entries")
        val apiKey = client.apiKey.firstOrNull()?.currentKey
            ?: error("$ASSET_NAME client is missing api_key.current_key")

        val options = FirebaseOptions.Builder()
            .setApplicationId(client.clientInfo.mobilesdkAppId)
            .setApiKey(apiKey)
            .setProjectId(cfg.projectInfo.projectId)
            .setGcmSenderId(cfg.projectInfo.projectNumber)
            .apply { cfg.projectInfo.storageBucket?.let { setStorageBucket(it) } }
            .build()

        FirebaseApp.initializeApp(appContext, options)
    }

    private const val ASSET_NAME = "google-services.json"
}

@Serializable
private data class GoogleServicesJson(
    @SerialName("project_info") val projectInfo: ProjectInfo,
    val client: List<Client>,
)

@Serializable
private data class ProjectInfo(
    @SerialName("project_number") val projectNumber: String,
    @SerialName("project_id") val projectId: String,
    @SerialName("storage_bucket") val storageBucket: String? = null,
)

@Serializable
private data class Client(
    @SerialName("client_info") val clientInfo: ClientInfo,
    @SerialName("api_key") val apiKey: List<ApiKey>,
)

@Serializable
private data class ClientInfo(
    @SerialName("mobilesdk_app_id") val mobilesdkAppId: String,
)

@Serializable
private data class ApiKey(
    @SerialName("current_key") val currentKey: String,
)

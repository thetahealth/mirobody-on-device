package ai.thetahealth.mirobody.data.config

import kotlinx.serialization.SerialName
import kotlinx.serialization.Serializable

/**
 * Mirrors the public `/mirobody.json` document served by every mirobody deployment.
 * Keys use the `__FOO__` convention because the same document is template-injected
 * into the web build at startup.
 */
@Serializable
data class ServerConfig(
    @SerialName("__IS_EHR_CONFIG_ON__") val isEhrConfigOn: Boolean = false,
    @SerialName("__IS_API_CONFIG_ON__") val isApiConfigOn: Boolean = false,
    @SerialName("__IS_QR_LOGIN_ON__") val isQrLoginOn: Boolean = false,
    @SerialName("__IS_GOOGLE_LOGIN_ON__") val isGoogleLoginOn: Boolean = false,
    @SerialName("__IS_APPLE_LOGIN_ON__") val isAppleLoginOn: Boolean = false,
    @SerialName("__IS_WECHAT_LOGIN_ON__") val isWechatLoginOn: Boolean = false,
    @SerialName("__IS_WEBAUTHN_ON__") val isWebauthnOn: Boolean = false,
    @SerialName("__IS_HIE_CONFIG_ON__") val isHieConfigOn: Boolean = false,
    @SerialName("__IS_MOBILE_SOURCE_ON__") val isMobileSourceOn: Boolean = false,
    @SerialName("__IS_NEW_FEATURES_ON__") val newFeaturesOn: List<String> = emptyList(),

    @SerialName("__FIREBASE_API_KEY__") val firebaseApiKey: String? = null,
    @SerialName("__FIREBASE_AUTH_DOMAIN__") val firebaseAuthDomain: String? = null,
    @SerialName("__FIREBASE_PROJECT_ID__") val firebaseProjectId: String? = null,
    @SerialName("__FIREBASE_STORAGE_BUCKET__") val firebaseStorageBucket: String? = null,
    @SerialName("__FIREBASE_MESSAGING_SENDER_ID__") val firebaseMessagingSenderId: String? = null,
    @SerialName("__FIREBASE_APP_ID__") val firebaseAppId: String? = null,
    @SerialName("__FIREBASE_MEASUREMENT_ID__") val firebaseMeasurementId: String? = null,
) {
    val hasFirebaseConfig: Boolean
        get() = !firebaseApiKey.isNullOrBlank() &&
            !firebaseProjectId.isNullOrBlank() &&
            !firebaseAppId.isNullOrBlank()
}
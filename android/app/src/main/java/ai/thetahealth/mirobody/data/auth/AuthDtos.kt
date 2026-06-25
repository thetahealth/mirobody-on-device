package ai.thetahealth.mirobody.data.auth

import kotlinx.serialization.SerialName
import kotlinx.serialization.Serializable

@Serializable
data class EmailLoginRequest(val email: String)

@Serializable
data class EmailLoginResponse(val email: String? = null)

@Serializable
data class EmailVerifyRequest(val email: String, val code: String)

@Serializable
data class FirebaseVerifyRequest(val token: String)

/**
 * WeChat OpenSDK login. `code` is the OAuth code from a SendAuth.Resp; `flow`
 * tells the backend to exchange it via sns/oauth2/access_token with the mobile
 * app credentials (WECHAT_APP_*).
 */
@Serializable
data class WechatVerifyRequest(val code: String, val flow: String = "app")

@Serializable
data class AuthTokenResponse(
    @SerialName("access_token") val accessToken: String,
    @SerialName("token_type") val tokenType: String = "Bearer",
    @SerialName("expires_in") val expiresIn: Long = 0,
    @SerialName("refresh_token") val refreshToken: String? = null,
    @SerialName("webauthn_registered") val webauthnRegistered: Boolean? = null,
    @SerialName("fallback_token") val fallbackToken: String? = null,
)
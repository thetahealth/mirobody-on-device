package ai.thetahealth.mirobody.data.auth

import ai.thetahealth.mirobody.data.net.ApiEnvelope
import retrofit2.http.Body
import retrofit2.http.POST

interface AuthApi {
    @POST("/email/login")
    suspend fun sendEmailCode(@Body body: EmailLoginRequest): ApiEnvelope<EmailLoginResponse>

    @POST("/email/verify")
    suspend fun verifyEmailCode(@Body body: EmailVerifyRequest): ApiEnvelope<AuthTokenResponse>

    /**
     * Exchanges a Firebase ID token for a mirobody JWT. The backend's
     * FirebaseTokenValidator accepts the token by its `iss`/`aud` and reads the
     * email regardless of which provider (Google, X, GitHub, …) issued it, so
     * every Firebase-brokered sign-in posts here.
     */
    @POST("/firebase/verify")
    suspend fun verifyFirebaseToken(@Body body: FirebaseVerifyRequest): ApiEnvelope<AuthTokenResponse>

    /**
     * Exchanges a WeChat OpenSDK OAuth code for a mirobody JWT. The backend
     * exchanges it via WeChat's sns/oauth2/access_token using the mobile-app
     * credentials (flow "app").
     */
    @POST("/wechat/verify")
    suspend fun verifyWechatCode(@Body body: WechatVerifyRequest): ApiEnvelope<AuthTokenResponse>
}
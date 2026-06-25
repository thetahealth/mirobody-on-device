package ai.thetahealth.mirobody.data.auth

import ai.thetahealth.mirobody.data.net.unwrap
import ai.thetahealth.mirobody.data.settings.SettingsStore

class AuthRepository(
    private val api: AuthApi,
    private val settings: SettingsStore,
) {
    suspend fun sendCode(email: String) {
        api.sendEmailCode(EmailLoginRequest(email = email)).unwrap()
        settings.setLastEmail(email)
    }

    suspend fun verifyCode(email: String, code: String): AuthTokenResponse {
        val token = api.verifyEmailCode(EmailVerifyRequest(email = email, code = code)).unwrap()
        settings.setAccessToken(token.accessToken)
        settings.setLastEmail(email)
        return token
    }

    suspend fun signOut() {
        settings.clearAuth()
    }
}
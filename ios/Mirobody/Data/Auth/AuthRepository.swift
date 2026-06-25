import Foundation

/// Email one-time-code auth. Mirrors `data/auth/AuthRepository.kt`. The Retrofit
/// `AuthApi` interface is folded into direct `ApiClient` calls here.
///
/// Endpoints: `POST /email/login`, `POST /email/verify`.
final class AuthRepository {
    private let api: ApiClient
    private let settings: SettingsStore

    init(api: ApiClient, settings: SettingsStore) {
        self.api = api
        self.settings = settings
    }

    func sendCode(email: String) async throws {
        let _: EmailLoginResponse = try await api.post("/email/login", EmailLoginRequest(email: email))
        settings.setLastEmail(email)
    }

    @discardableResult
    func verifyCode(email: String, code: String) async throws -> AuthTokenResponse {
        let token: AuthTokenResponse = try await api.post(
            "/email/verify",
            EmailVerifyRequest(email: email, code: code)
        )
        settings.setAccessToken(token.accessToken)
        settings.setLastEmail(email)
        return token
    }

    func signOut() {
        settings.clearAuth()
    }
}

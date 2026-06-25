import Foundation
import FirebaseAuth

enum GitHubAuthError: LocalizedError {
    case notConfigured
    case noCredential
    case noIdToken

    var errorDescription: String? {
        switch self {
        case .notConfigured: return "GitHub sign-in is not configured for iOS yet."
        case .noCredential:  return "GitHub sign-in returned no credential."
        case .noIdToken:     return "Firebase user has no ID token."
        }
    }
}

/// "Sign in with GitHub" via Firebase Auth — mirrors `GoogleAuthRepository`,
/// differing only in the OAuth provider id ("github.com").
///
/// Flow: Firebase opens a web OAuth session against `github.com`
/// (`OAuthProvider("github.com")`), we fetch the resulting Firebase ID token, POST
/// it to the backend's `/firebase/verify` (which routes through the Firebase
/// validator — it accepts any Firebase ID token by its `iss`/`aud` and reads the
/// email, regardless of provider — and returns a mirobody JWT), then sign out of
/// Firebase locally. The backend's native `/github/verify` does its own
/// server-side code exchange and serves the web client; on iOS the Firebase route
/// is the idiomatic one and needs no GitHub client secret on the device.
///
/// Requires "GitHub" enabled in the Firebase console (Authentication ->
/// Sign-in method -> GitHub), with the GitHub OAuth app's id/secret set there.
@MainActor
final class GitHubAuthRepository {
    private let api: ApiClient
    private let settings: SettingsStore

    init(api: ApiClient, settings: SettingsStore) {
        self.api = api
        self.settings = settings
    }

    /// Hidden in the UI until the iOS Firebase values are filled in
    /// (see `FirebaseInitializer`).
    var isAvailable: Bool { FirebaseInitializer.isConfigured }

    @discardableResult
    func signInWithGithub() async throws -> AuthTokenResponse {
        guard FirebaseInitializer.isConfigured else { throw GitHubAuthError.notConfigured }
        let auth = Auth.auth()
        let provider = OAuthProvider(providerID: "github.com")
        // Match the web flow's scope so the verified token carries an email.
        provider.scopes = ["read:user", "user:email"]

        // Firebase presents the web OAuth flow and hands back a credential.
        let credential: AuthCredential = try await withCheckedThrowingContinuation { cont in
            provider.getCredentialWith(nil) { credential, error in
                if let error {
                    cont.resume(throwing: error)
                } else if let credential {
                    cont.resume(returning: credential)
                } else {
                    cont.resume(throwing: GitHubAuthError.noCredential)
                }
            }
        }

        let result = try await auth.signIn(with: credential)
        let firebaseIdToken = try await result.user.getIDToken()

        let backendToken: AuthTokenResponse = try await api.post(
            "/firebase/verify",
            FirebaseVerifyRequest(token: firebaseIdToken)
        )
        settings.setAccessToken(backendToken.accessToken)
        if let email = result.user.email { settings.setLastEmail(email) }

        // The backend JWT is authoritative; we don't keep a Firebase session.
        try? auth.signOut()
        return backendToken
    }
}

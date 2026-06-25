import Foundation
import FirebaseAuth

enum GoogleAuthError: LocalizedError {
    case notConfigured
    case noCredential
    case noIdToken

    var errorDescription: String? {
        switch self {
        case .notConfigured: return "Google sign-in is not configured for iOS yet."
        case .noCredential:  return "Google sign-in returned no credential."
        case .noIdToken:     return "Firebase user has no ID token."
        }
    }
}

/// Web-flow Google sign-in via Firebase Auth — mirrors `GoogleAuthRepository.kt`.
///
/// Flow: Firebase opens a web OAuth session against `accounts.google.com`
/// (`OAuthProvider("google.com")`), we fetch the resulting Firebase ID token, POST
/// it to the backend's `/firebase/verify` (which routes through the Firebase
/// validator and returns a mirobody JWT), then sign out of Firebase locally — the
/// backend JWT is the authoritative session.
@MainActor
final class GoogleAuthRepository {
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
    func signInWithGoogle() async throws -> AuthTokenResponse {
        guard FirebaseInitializer.isConfigured else { throw GoogleAuthError.notConfigured }
        let auth = Auth.auth()
        let provider = OAuthProvider(providerID: "google.com")

        // Firebase presents the web OAuth flow and hands back a credential.
        let credential: AuthCredential = try await withCheckedThrowingContinuation { cont in
            provider.getCredentialWith(nil) { credential, error in
                if let error {
                    cont.resume(throwing: error)
                } else if let credential {
                    cont.resume(returning: credential)
                } else {
                    cont.resume(throwing: GoogleAuthError.noCredential)
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

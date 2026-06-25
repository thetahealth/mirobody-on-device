import Foundation
import FirebaseAuth

enum XAuthError: LocalizedError {
    case notConfigured
    case noCredential
    case noIdToken

    var errorDescription: String? {
        switch self {
        case .notConfigured: return "X sign-in is not configured for iOS yet."
        case .noCredential:  return "X sign-in returned no credential."
        case .noIdToken:     return "Firebase user has no ID token."
        }
    }
}

/// "Sign in with X" (formerly Twitter) via Firebase Auth — mirrors
/// `GoogleAuthRepository` / `GitHubAuthRepository`, differing only in the OAuth
/// provider id ("twitter.com", Firebase's built-in id for X).
///
/// Flow: Firebase opens a web OAuth session against X
/// (`OAuthProvider("twitter.com")`), we fetch the resulting Firebase ID token,
/// POST it to the backend's `/firebase/verify` (which routes through the Firebase
/// validator — it accepts any Firebase ID token by its `iss`/`aud` and reads the
/// email, regardless of provider — and returns a mirobody JWT), then sign out of
/// Firebase locally. There is no native `/x/verify`: every platform uses this
/// Firebase route, so a user gets one account keyed by the same email everywhere.
///
/// Requires "Twitter" enabled in the Firebase console (Authentication ->
/// Sign-in method -> Twitter), with the X app's API key/secret set there and
/// "Request email address from users" turned on (X only releases an email then).
@MainActor
final class XAuthRepository {
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
    func signInWithX() async throws -> AuthTokenResponse {
        guard FirebaseInitializer.isConfigured else { throw XAuthError.notConfigured }
        let auth = Auth.auth()
        let provider = OAuthProvider(providerID: "twitter.com")

        // Firebase presents the web OAuth flow and hands back a credential.
        let credential: AuthCredential = try await withCheckedThrowingContinuation { cont in
            provider.getCredentialWith(nil) { credential, error in
                if let error {
                    cont.resume(throwing: error)
                } else if let credential {
                    cont.resume(returning: credential)
                } else {
                    cont.resume(throwing: XAuthError.noCredential)
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

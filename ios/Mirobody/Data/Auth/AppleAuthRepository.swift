import AuthenticationServices
import UIKit

enum AppleAuthError: LocalizedError {
    case noIdToken
    case cancelled

    var errorDescription: String? {
        switch self {
        case .noIdToken:  return "Apple sign-in returned no identity token."
        case .cancelled:  return "Apple sign-in was cancelled."
        }
    }
}

/// Native "Sign in with Apple" via `ASAuthorizationController` — the iOS
/// counterpart of the web client's Apple JS flow.
///
/// Flow: request an Apple ID credential (scopes name + email); Apple hands back a
/// short-lived `identityToken` (a JWT signed by Apple). POST it to `/apple/verify`,
/// which validates it against Apple's JWKS (`AppleTokenValidator`) and returns a
/// mirobody JWT. Unlike Google, no Firebase is involved.
///
/// Requires the "Sign in with Apple" capability/entitlement (see project.yml) and
/// the bundle id registered under the Apple Services ID configured as APPLE_CLIENT_ID
/// on the backend.
@MainActor
final class AppleAuthRepository: NSObject {
    private let api: ApiClient
    private let settings: SettingsStore
    private var continuation: CheckedContinuation<String, Error>?

    init(api: ApiClient, settings: SettingsStore) {
        self.api = api
        self.settings = settings
    }

    /// Apple sign-in is a system capability, always available on iOS 13+.
    var isAvailable: Bool { true }

    @discardableResult
    func signInWithApple() async throws -> AuthTokenResponse {
        let idToken = try await requestIdentityToken()
        let backendToken: AuthTokenResponse = try await api.post(
            "/apple/verify",
            AppleVerifyRequest(idToken: idToken)
        )
        settings.setAccessToken(backendToken.accessToken)
        return backendToken
    }

    private func requestIdentityToken() async throws -> String {
        try await withCheckedThrowingContinuation { cont in
            self.continuation = cont
            let request = ASAuthorizationAppleIDProvider().createRequest()
            request.requestedScopes = [.fullName, .email]
            let controller = ASAuthorizationController(authorizationRequests: [request])
            controller.delegate = self
            controller.presentationContextProvider = self
            controller.performRequests()
        }
    }
}

extension AppleAuthRepository: ASAuthorizationControllerDelegate {
    func authorizationController(
        controller: ASAuthorizationController,
        didCompleteWithAuthorization authorization: ASAuthorization
    ) {
        guard
            let credential = authorization.credential as? ASAuthorizationAppleIDCredential,
            let tokenData = credential.identityToken,
            let token = String(data: tokenData, encoding: .utf8)
        else {
            continuation?.resume(throwing: AppleAuthError.noIdToken)
            continuation = nil
            return
        }
        // `email` is only delivered on the first authorization; keep it for the
        // login-screen prefill. The backend reads the email from the token itself.
        if let email = credential.email { settings.setLastEmail(email) }
        continuation?.resume(returning: token)
        continuation = nil
    }

    func authorizationController(
        controller: ASAuthorizationController,
        didCompleteWithError error: Error
    ) {
        if let authError = error as? ASAuthorizationError, authError.code == .canceled {
            continuation?.resume(throwing: AppleAuthError.cancelled)
        } else {
            continuation?.resume(throwing: error)
        }
        continuation = nil
    }
}

extension AppleAuthRepository: ASAuthorizationControllerPresentationContextProviding {
    func presentationAnchor(for controller: ASAuthorizationController) -> ASPresentationAnchor {
        let scene = UIApplication.shared.connectedScenes
            .compactMap { $0 as? UIWindowScene }
            .first { $0.activationState == .foregroundActive }
        return scene?.keyWindow ?? ASPresentationAnchor()
    }
}

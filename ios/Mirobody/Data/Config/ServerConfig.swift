import Foundation

/// Mirrors the `GET /auth/providers` document: which federated sign-ins this
/// deployment has configured, plus each one's public config. Mirrors
/// `data/config/ServerConfig.kt`.
///
/// This replaced a `/mirobody.json` document that no mirobody server has ever
/// served — a leftover from the template-injected web build of the old backend,
/// so the fetch 404'd on every launch. It also carried a dozen unrelated flags
/// (EHR, QR, WebAuthn, HIE, feature list) that nothing read; they are gone.
///
/// A provider the server has no opinion about is ABSENT rather than disabled,
/// hence the optionals.
struct ServerConfig: Codable {
    var google: Provider?
    var apple: Provider?
    var wechat: Provider?
    var github: Provider?
    var tanka: Provider?

    struct Provider: Codable {
        var enabled = false
        /// Public config; present only when enabled, and only if there is any.
        var config: ProviderConfig?
        /// Whether the NATIVE flow works — a different question from `enabled`,
        /// and the one this client must ask. Sent only where the two can differ:
        ///
        ///  - google: the web button needs FIREBASE_WEB_API_KEY to boot the JS
        ///    SDK; verifying an ID token needs only FIREBASE_PROJECT_ID. (X rides
        ///    the same Firebase project, so it reads this too.)
        ///  - wechat: two independent registrations — an Open Platform *website*
        ///    app for the browser, a *mobile application* for the OpenSDK. This
        ///    client uses the latter (POST /wechat/verify with flow:"app").
        var appEnabled = false
    }

    /// Union of the per-provider public configs: Firebase keys for google,
    /// `clientId` for apple/github, `appid` for wechat, nothing for tanka.
    struct ProviderConfig: Codable {
        var apiKey: String?
        var projectId: String?
        var messagingSenderId: String?
        var clientId: String?
        var appid: String?
    }

    // Accessors are named for the VERIFICATION PATH, not the provider, because on
    // this client several buttons share one path and the server prerequisite is the
    // path's, not the provider's. Nothing reads them yet — the sign-in screen still
    // gates purely on client capability (Firebase bundled? WeChat appid bundled?).
    // Wiring them in is a deliberate follow-up: it needs a build to verify, and
    // getting it wrong hides every sign-in button.

    /// The server can verify a Firebase ID token — it needs only
    /// FIREBASE_PROJECT_ID, which is what `google.appEnabled` reports.
    /// On iOS this covers Google, X *and* GitHub: all three go through Firebase's
    /// OAuthProvider and POST to /firebase/verify. GITHUB_CLIENT_ID gates the
    /// browser's server-side exchange, not this.
    var firebaseVerifyEnabled: Bool { google?.appEnabled == true }

    /// The native Apple path: a real Apple id_token POSTed to /apple/verify, which
    /// needs APPLE_CLIENT_ID to validate the `aud` claim. This is iOS's Apple flow
    /// (ASAuthorizationController); Android's rides Firebase instead.
    var appleNativeEnabled: Bool { apple?.enabled == true }

    /// The mobile OpenSDK flow (POST /wechat/verify with flow:"app"), not the web one.
    var wechatEnabled: Bool { wechat?.appEnabled == true }

    var tankaEnabled: Bool { tanka?.enabled == true }
}

import Foundation
import FirebaseCore

/// Configures the default `FirebaseApp` in code, mirroring Android's
/// `FirebaseInitializer.kt` (which parses `google-services.json` and builds
/// `FirebaseOptions` explicitly rather than relying on the auto-init plugin).
///
/// We deliberately do NOT ship a `GoogleService-Info.plist`: a placeholder plist
/// would crash `FirebaseApp.configure()` at launch. Instead we configure only when
/// the two iOS-specific values below have been filled in.
///
/// ── REQUIRED ONE-TIME SETUP for Google sign-in (email login needs none of this) ──
/// The Android `google-services.json` has no iOS app, so two values are unknown
/// here. In the Firebase console (project `mirobody-81000`):
///   1. Add an iOS app with bundle id `ai.thetahealth.mirobody`.
///   2. Copy `GOOGLE_APP_ID` (looks like `1:555773021699:ios:...`) into `iosAppID`.
///   3. Copy the iOS OAuth `CLIENT_ID` (`...apps.googleusercontent.com`) into
///      `oauthClientID`, and put its REVERSED form into the `CFBundleURLSchemes`
///      entry in project.yml (replace `REVERSED_CLIENT_ID`).
/// Until then `isConfigured` stays false and the Google button is hidden.
enum FirebaseInitializer {

    // Shared values, lifted verbatim from android/.../assets/google-services.json.
    static let projectID = "mirobody-81000"
    static let projectNumber = "555773021699"               // = GCM sender id
    static let apiKey = "AIzaSyCGuHqukiVwiClH0b1HYpW4I1bGdK9O-1w"
    static let storageBucket = "mirobody-81000.firebasestorage.app"
    static let bundleID = "ai.thetahealth.mirobody"

    // iOS-specific — fill in from the Firebase console (see header). Sentinel values
    // mean "not configured yet".
    static let iosAppID = "TODO_IOS_GOOGLE_APP_ID"          // 1:555773021699:ios:...
    static let oauthClientID = "TODO_IOS_OAUTH_CLIENT_ID"   // ...apps.googleusercontent.com

    /// True once the iOS-specific values have been supplied.
    static var isConfigured: Bool {
        !iosAppID.hasPrefix("TODO_") && !oauthClientID.hasPrefix("TODO_")
    }

    private static var didConfigure = false

    static func ensureInitialized() {
        guard isConfigured, !didConfigure, FirebaseApp.app() == nil else { return }
        let options = FirebaseOptions(googleAppID: iosAppID, gcmSenderID: projectNumber)
        options.apiKey = apiKey
        options.projectID = projectID
        options.storageBucket = storageBucket
        options.bundleID = bundleID
        options.clientID = oauthClientID
        FirebaseApp.configure(options: options)
        didConfigure = true
    }
}

import Foundation

/// Mirrors the public `/mirobody.json` document served by every deployment. Keys
/// use the `__FOO__` convention because the same document is template-injected
/// into the web build. Mirrors `data/config/ServerConfig.kt`.
struct ServerConfig: Codable {
    var isEhrConfigOn = false
    var isApiConfigOn = false
    var isQrLoginOn = false
    var isGoogleLoginOn = false
    var isAppleLoginOn = false
    var isWechatLoginOn = false
    var isWebauthnOn = false
    var isHieConfigOn = false
    var isMobileSourceOn = false
    var newFeaturesOn: [String] = []

    var firebaseApiKey: String?
    var firebaseAuthDomain: String?
    var firebaseProjectId: String?
    var firebaseStorageBucket: String?
    var firebaseMessagingSenderId: String?
    var firebaseAppId: String?
    var firebaseMeasurementId: String?

    var hasFirebaseConfig: Bool {
        !(firebaseApiKey ?? "").isEmpty &&
        !(firebaseProjectId ?? "").isEmpty &&
        !(firebaseAppId ?? "").isEmpty
    }

    enum CodingKeys: String, CodingKey {
        case isEhrConfigOn = "__IS_EHR_CONFIG_ON__"
        case isApiConfigOn = "__IS_API_CONFIG_ON__"
        case isQrLoginOn = "__IS_QR_LOGIN_ON__"
        case isGoogleLoginOn = "__IS_GOOGLE_LOGIN_ON__"
        case isAppleLoginOn = "__IS_APPLE_LOGIN_ON__"
        case isWechatLoginOn = "__IS_WECHAT_LOGIN_ON__"
        case isWebauthnOn = "__IS_WEBAUTHN_ON__"
        case isHieConfigOn = "__IS_HIE_CONFIG_ON__"
        case isMobileSourceOn = "__IS_MOBILE_SOURCE_ON__"
        case newFeaturesOn = "__IS_NEW_FEATURES_ON__"
        case firebaseApiKey = "__FIREBASE_API_KEY__"
        case firebaseAuthDomain = "__FIREBASE_AUTH_DOMAIN__"
        case firebaseProjectId = "__FIREBASE_PROJECT_ID__"
        case firebaseStorageBucket = "__FIREBASE_STORAGE_BUCKET__"
        case firebaseMessagingSenderId = "__FIREBASE_MESSAGING_SENDER_ID__"
        case firebaseAppId = "__FIREBASE_APP_ID__"
        case firebaseMeasurementId = "__FIREBASE_MEASUREMENT_ID__"
    }

    init(from decoder: Decoder) throws {
        let c = try decoder.container(keyedBy: CodingKeys.self)
        func flag(_ k: CodingKeys) -> Bool { (try? c.decode(Bool.self, forKey: k)) ?? false }
        func str(_ k: CodingKeys) -> String? { try? c.decode(String.self, forKey: k) }
        isEhrConfigOn = flag(.isEhrConfigOn)
        isApiConfigOn = flag(.isApiConfigOn)
        isQrLoginOn = flag(.isQrLoginOn)
        isGoogleLoginOn = flag(.isGoogleLoginOn)
        isAppleLoginOn = flag(.isAppleLoginOn)
        isWechatLoginOn = flag(.isWechatLoginOn)
        isWebauthnOn = flag(.isWebauthnOn)
        isHieConfigOn = flag(.isHieConfigOn)
        isMobileSourceOn = flag(.isMobileSourceOn)
        newFeaturesOn = (try? c.decode([String].self, forKey: .newFeaturesOn)) ?? []
        firebaseApiKey = str(.firebaseApiKey)
        firebaseAuthDomain = str(.firebaseAuthDomain)
        firebaseProjectId = str(.firebaseProjectId)
        firebaseStorageBucket = str(.firebaseStorageBucket)
        firebaseMessagingSenderId = str(.firebaseMessagingSenderId)
        firebaseAppId = str(.firebaseAppId)
        firebaseMeasurementId = str(.firebaseMeasurementId)
    }
}

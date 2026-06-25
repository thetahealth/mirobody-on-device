import Foundation

/// Request/response DTOs for the auth endpoints. Mirrors `data/auth/AuthDtos.kt`.

struct EmailLoginRequest: Encodable {
    let email: String
}

struct EmailLoginResponse: Decodable {
    let email: String?
}

struct EmailVerifyRequest: Encodable {
    let email: String
    let code: String
}

struct FirebaseVerifyRequest: Encodable {
    let token: String
}

/// Apple ID token from `ASAuthorizationController`, verified by `/apple/verify`.
struct AppleVerifyRequest: Encodable {
    let idToken: String
    enum CodingKeys: String, CodingKey { case idToken = "id_token" }
}

/// WeChat OpenSDK OAuth code; `flow=app` selects the mobile-app credentials on
/// the backend's `/wechat/verify`.
struct WechatVerifyRequest: Encodable {
    let code: String
    let flow: String
}

struct AuthTokenResponse: Decodable {
    let accessToken: String
    let tokenType: String
    let expiresIn: Int
    let refreshToken: String?
    let webauthnRegistered: Bool?
    let fallbackToken: String?

    enum CodingKeys: String, CodingKey {
        case accessToken = "access_token"
        case tokenType = "token_type"
        case expiresIn = "expires_in"
        case refreshToken = "refresh_token"
        case webauthnRegistered = "webauthn_registered"
        case fallbackToken = "fallback_token"
    }

    init(from decoder: Decoder) throws {
        let c = try decoder.container(keyedBy: CodingKeys.self)
        accessToken = try c.decode(String.self, forKey: .accessToken)
        tokenType = try c.decodeIfPresent(String.self, forKey: .tokenType) ?? "Bearer"
        expiresIn = try c.decodeIfPresent(Int.self, forKey: .expiresIn) ?? 0
        refreshToken = try c.decodeIfPresent(String.self, forKey: .refreshToken)
        webauthnRegistered = try c.decodeIfPresent(Bool.self, forKey: .webauthnRegistered)
        fallbackToken = try c.decodeIfPresent(String.self, forKey: .fallbackToken)
    }
}

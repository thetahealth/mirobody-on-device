import Foundation

// "Sign in with WeChat" via the WeChat OpenSDK.
//
// The Tencent SDK is NOT a Swift Package, so it can't be declared in project.yml
// and isn't present in a default checkout. Everything that touches it is therefore
// compiled only when the `WechatOpenSDK` module is linkable — drop the framework
// in (see ios/README.md) and this activates automatically. Without it, the repo
// is an inert stub whose `isAvailable` is false, so the login screen hides the
// WeChat button and the project still builds cleanly.
//
// SDK flow (when present): register the Open Platform "Mobile Application" appid
// (+ universal link) -> send a SendAuthReq (scope snsapi_userinfo) -> WeChat opens
// and returns via the app's universal link / URL scheme -> `WXApi.handleOpen`
// dispatches `onResp` here with the OAuth code -> POST /wechat/verify {flow:"app"}.

enum WeChatAuthError: LocalizedError {
    case notConfigured
    case notInstalled
    case cancelled
    case failed(Int32)

    var errorDescription: String? {
        switch self {
        case .notConfigured: return "WeChat sign-in is not configured."
        case .notInstalled:  return "WeChat is not installed."
        case .cancelled:     return "WeChat sign-in was cancelled."
        case .failed(let c): return "WeChat sign-in failed (\(c))."
        }
    }
}

#if canImport(WechatOpenSDK)
import WechatOpenSDK

@MainActor
final class WeChatAuthRepository: NSObject, WXApiDelegate {
    private let api: ApiClient
    private let settings: SettingsStore
    private var continuation: CheckedContinuation<String, Error>?

    // Read from Info.plist (set these when you add the SDK — see ios/README.md).
    private let appId: String
    private let universalLink: String
    private var registered = false

    init(api: ApiClient, settings: SettingsStore) {
        self.api = api
        self.settings = settings
        self.appId = (Bundle.main.object(forInfoDictionaryKey: "WechatAppID") as? String) ?? ""
        self.universalLink = (Bundle.main.object(forInfoDictionaryKey: "WechatUniversalLink") as? String) ?? ""
        super.init()
    }

    var isAvailable: Bool { !appId.isEmpty }

    private func ensureRegistered() {
        guard !registered, !appId.isEmpty else { return }
        registered = WXApi.registerApp(appId, universalLink: universalLink)
    }

    @discardableResult
    func signInWithWeChat() async throws -> AuthTokenResponse {
        guard !appId.isEmpty else { throw WeChatAuthError.notConfigured }
        ensureRegistered()
        guard WXApi.isWXAppInstalled() else { throw WeChatAuthError.notInstalled }

        let code = try await requestCode()
        let backendToken: AuthTokenResponse = try await api.post(
            "/wechat/verify",
            WechatVerifyRequest(code: code, flow: "app")
        )
        settings.setAccessToken(backendToken.accessToken)
        return backendToken
    }

    /// Called from `MirobodyApp.onOpenURL` so WeChat's callback reaches the SDK.
    func handleOpenURL(_ url: URL) -> Bool {
        WXApi.handleOpen(url, delegate: self)
    }

    private func requestCode() async throws -> String {
        try await withCheckedThrowingContinuation { cont in
            self.continuation = cont
            let req = SendAuthReq()
            req.scope = "snsapi_userinfo"
            req.state = "mirobody_wechat"
            WXApi.send(req)
        }
    }

    // MARK: WXApiDelegate

    func onReq(_ req: BaseReq) {}

    func onResp(_ resp: BaseResp) {
        guard let auth = resp as? SendAuthResp else { return }
        switch auth.errCode {
        case 0:
            continuation?.resume(returning: auth.code ?? "")
        case -2:   // WXErrCodeUserCancel
            continuation?.resume(throwing: WeChatAuthError.cancelled)
        case -4:   // WXErrCodeAuthDeny
            continuation?.resume(throwing: WeChatAuthError.cancelled)
        default:
            continuation?.resume(throwing: WeChatAuthError.failed(auth.errCode))
        }
        continuation = nil
    }
}

#else

/// Stub used when the WeChat SDK isn't linked — keeps the app buildable and the
/// WeChat button hidden (`isAvailable == false`).
@MainActor
final class WeChatAuthRepository {
    init(api: ApiClient, settings: SettingsStore) {}
    var isAvailable: Bool { false }

    @discardableResult
    func signInWithWeChat() async throws -> AuthTokenResponse {
        throw WeChatAuthError.notConfigured
    }

    func handleOpenURL(_ url: URL) -> Bool { false }
}

#endif

import Foundation

/// Renders an `AppError` as a localized, user-facing string. Mirrors
/// `ui/AppErrorMessage.kt`.
func localizedMessage(_ error: AppError, language: String) -> String {
    switch error {
    case .auth:
        return L("error_auth", language)
    case .network:
        return L("error_network", language)
    case .http(let status, _):
        return L("error_http", language, status)
    case .api(let code, let serverMessage, _):
        let suffix = serverMessage?.nonBlank.map { ": \($0)" } ?? ""
        return L("error_api", language, code, suffix)
    case .parse:
        return L("error_parse", language)
    case .unknown:
        return L("error_unknown", language)
    }
}

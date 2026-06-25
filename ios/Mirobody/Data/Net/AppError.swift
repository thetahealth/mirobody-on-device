import Foundation

/// App-level error taxonomy — mirrors `data/net/AppError.kt`. Network / decoding /
/// HTTP / API-envelope failures are normalized into one of these so the UI can
/// render a localized message and decide on follow-up (route to login on `.auth`).
enum AppError: Error {
    /// HTTP 401/403 — auth expired/invalid. Caller should route back to login.
    case auth(underlying: Error?)
    /// Any other non-2xx HTTP status.
    case http(status: Int, underlying: Error?)
    /// Envelope returned with code != 0. `serverMessage` is the backend message.
    case api(code: Int, serverMessage: String?, underlying: Error?)
    /// JSON missing/extra fields, wrong type, malformed payload.
    case parse(underlying: Error?)
    /// Connectivity / timeout / unreachable host.
    case network(underlying: Error?)
    /// Anything we didn't categorize.
    case unknown(underlying: Error?)
}

/// Thrown by `ApiClient` for non-2xx responses before envelope parsing.
struct HTTPStatusError: Error {
    let status: Int
}

extension Error {
    func toAppError() -> AppError {
        switch self {
        case let e as AppError:
            return e
        case let e as ApiError:
            return .api(code: e.apiCode, serverMessage: e.message, underlying: e)
        case let e as HTTPStatusError:
            return (e.status == 401 || e.status == 403) ? .auth(underlying: e)
                                                        : .http(status: e.status, underlying: e)
        case is DecodingError:
            return .parse(underlying: self)
        case let e as URLError:
            return .network(underlying: e)
        default:
            return .unknown(underlying: self)
        }
    }
}

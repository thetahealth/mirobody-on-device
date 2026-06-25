import Foundation

/// Wire envelope used by most of the API: `{ code, message, data }`. Mirrors
/// `data/net/ApiEnvelope.kt`.
struct ApiEnvelope<T: Decodable>: Decodable {
    let code: Int
    let message: String?
    let data: T?

    enum CodingKeys: String, CodingKey { case code, message, data }

    init(from decoder: Decoder) throws {
        let c = try decoder.container(keyedBy: CodingKeys.self)
        self.code = try c.decodeIfPresent(Int.self, forKey: .code) ?? 0
        self.message = try c.decodeIfPresent(String.self, forKey: .message)
        self.data = try c.decodeIfPresent(T.self, forKey: .data)
    }
}

/// Thrown when an envelope returns a non-zero `code`.
struct ApiError: Error {
    let apiCode: Int
    let message: String?
}

extension ApiEnvelope {
    /// Returns `data` or throws — non-zero code or missing data is an error.
    func unwrap() throws -> T {
        if code != 0 { throw ApiError(apiCode: code, message: message) }
        guard let data else { throw ApiError(apiCode: code, message: message ?? "empty response data") }
        return data
    }

    /// Validates the code without requiring a payload (for delete-style calls).
    func ensureOk() throws {
        if code != 0 { throw ApiError(apiCode: code, message: message) }
    }
}

import Foundation

/// HTTP client for the JSON API — the iOS analogue of the Retrofit + OkHttp stack
/// on Android. It folds the three OkHttp interceptors into one place:
///
///  - **Base URL**: every request is built against the URL currently configured in
///    settings (`BaseUrlInterceptor`). There is no fixed host.
///  - **Auth**: a `Bearer` token is attached when present (`AuthInterceptor`).
///  - **401 handling**: a 401 clears the persisted token so the UI routes back to
///    login (`UnauthorizedInterceptor`).
///
/// Responses are wrapped in `ApiEnvelope` except `/mirobody.json`, which is raw —
/// use `getRaw` for that.
final class ApiClient {
    private let settings: SettingsStore
    private let session: URLSession
    private let decoder = JSONDecoder()
    private let encoder = JSONEncoder()

    init(settings: SettingsStore) {
        self.settings = settings
        let cfg = URLSessionConfiguration.default
        cfg.timeoutIntervalForRequest = 60          // matches OkHttp readTimeout
        cfg.waitsForConnectivity = false
        self.session = URLSession(configuration: cfg)
    }

    // MARK: Request building (shared with the SSE stream client)

    /// Builds a `URLRequest` against the configured base URL with auth attached.
    /// `path` includes the leading slash and any query string, e.g. `/api/history?page=0`.
    func makeURLRequest(
        path: String,
        method: String,
        jsonBody: Data? = nil,
        accept: String = "application/json"
    ) throws -> URLRequest {
        let base = settings.baseURLValue.trimmingCharacters(in: .whitespaces)
        guard let url = URL(string: base + path) else {
            throw URLError(.badURL)
        }
        var req = URLRequest(url: url)
        req.httpMethod = method
        req.setValue(accept, forHTTPHeaderField: "Accept")
        if let token = settings.tokenValue, !token.isEmpty {
            req.setValue("Bearer \(token)", forHTTPHeaderField: "Authorization")
        }
        if let jsonBody {
            req.httpBody = jsonBody
            req.setValue("application/json", forHTTPHeaderField: "Content-Type")
        }
        return req
    }

    func encode<B: Encodable>(_ body: B) throws -> Data {
        try encoder.encode(body)
    }

    // MARK: Typed calls

    /// GET an enveloped endpoint and unwrap its `data`.
    func get<T: Decodable>(_ path: String) async throws -> T {
        let data = try await send(makeURLRequest(path: path, method: "GET"))
        return try decoder.decode(ApiEnvelope<T>.self, from: data).unwrap()
    }

    /// POST a body to an enveloped endpoint and unwrap its `data`.
    func post<T: Decodable, B: Encodable>(_ path: String, _ body: B) async throws -> T {
        let data = try await send(makeURLRequest(path: path, method: "POST", jsonBody: try encoder.encode(body)))
        return try decoder.decode(ApiEnvelope<T>.self, from: data).unwrap()
    }

    /// POST a body to an enveloped endpoint that returns no useful data; just
    /// validate the code (e.g. history delete).
    func postEnsureOk<B: Encodable>(_ path: String, _ body: B) async throws {
        let data = try await send(makeURLRequest(path: path, method: "POST", jsonBody: try encoder.encode(body)))
        try decoder.decode(ApiEnvelope<DiscardableData>.self, from: data).ensureOk()
    }

    /// GET a non-enveloped (raw) JSON document, e.g. `/mirobody.json`.
    func getRaw<T: Decodable>(_ path: String) async throws -> T {
        let data = try await send(makeURLRequest(path: path, method: "GET"))
        return try decoder.decode(T.self, from: data)
    }

    /// POST raw JSON to a non-enveloped endpoint and return its raw body. Used for
    /// `/fhir`, which speaks FHIR resources / OperationOutcome rather than the
    /// `{code,msg,data}` envelope. Throws `HTTPStatusError` on a non-2xx status.
    @discardableResult
    func postRaw(_ path: String, jsonBody: Data, accept: String = "application/json") async throws -> Data {
        try await send(makeURLRequest(path: path, method: "POST", jsonBody: jsonBody, accept: accept))
    }

    // MARK: Transport

    private func send(_ request: URLRequest) async throws -> Data {
        let (data, response): (Data, URLResponse)
        do {
            (data, response) = try await session.data(for: request)
        } catch {
            throw error   // URLError → mapped to .network by toAppError()
        }
        guard let http = response as? HTTPURLResponse else {
            throw URLError(.badServerResponse)
        }
        if http.statusCode == 401 {
            settings.clearAuth()   // mirrors UnauthorizedInterceptor (401 only)
        }
        guard (200..<300).contains(http.statusCode) else {
            throw HTTPStatusError(status: http.statusCode)
        }
        return data
    }
}

/// Decodes-and-discards any `data` payload for endpoints whose body we ignore.
struct DiscardableData: Decodable {
    init(from decoder: Decoder) throws {}
}

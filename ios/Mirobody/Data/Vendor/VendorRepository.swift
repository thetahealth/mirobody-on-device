import Foundation

/// Vendor account management (the `/vendors` endpoints) — mirrors
/// `data/vendor/VendorRepository.kt`. The Retrofit `VendorApi` interface is folded
/// into direct `ApiClient` calls here.
///
/// Connect is a browser hand-off: GET authorize → open the URL; the vendor redirects
/// to the server callback, which stores the grant, so the app just re-lists on
/// return. Icons are public (server-resolved, same-origin) so they carry no
/// third-party call. Bearer auth is added by `ApiClient`.
final class VendorRepository {
    private let api: ApiClient

    init(api: ApiClient) {
        self.api = api
    }

    /// The user's connected vendors, keyed by id.
    func connected() async throws -> [String: VendorLink] {
        let links: [VendorLink] = try await api.get("/vendors")
        return Dictionary(links.map { ($0.id, $0) }, uniquingKeysWith: { _, last in last })
    }

    /// Server-resolved brand icons `{ id -> data URI }`. Best-effort (empty on error).
    func icons() async -> [String: String] {
        (try? await api.get("/vendors/icons")) ?? [:]
    }

    /// The vendor's OAuth consent URL to open in a browser, or nil.
    func authorizeUrl(id: String) async throws -> String? {
        let enc = id.addingPercentEncoding(withAllowedCharacters: .urlPathAllowed) ?? id
        let resp: VendorAuthorize = try await api.get("/vendors/\(enc)/authorize")
        return resp.authorizeUrl
    }

    /// Revoke the grant + delete stored tokens for a vendor.
    func unlink(id: String) async throws {
        let enc = id.addingPercentEncoding(withAllowedCharacters: .urlPathAllowed) ?? id
        try await api.postEnsureOk("/vendors/\(enc)/unlink", EmptyBody())
    }
}

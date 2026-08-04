import Foundation

/// EHR connect (SMART on FHIR) — mirrors `data/health/EhrRepository.kt`. The
/// Retrofit `EhrApi` interface is folded into direct `ApiClient` calls here, as the
/// other iOS repositories do.
///
/// Connect is a browser hand-off: POST authorize → open the URL; the EHR redirects
/// to the server callback, which exchanges the code and stores the link. The app
/// then pulls records with `sync`. Bearer auth is added by `ApiClient`.
final class EhrRepository {
    private let api: ApiClient

    init(api: ApiClient) {
        self.api = api
    }

    /// Search the EHR provider directory.
    func providers(query: String) async throws -> [EhrProvider] {
        let q = query.addingPercentEncoding(withAllowedCharacters: .urlQueryValueAllowed) ?? ""
        let resp: EhrProviders = try await api.get("/health/ehr/providers?q=\(q)")
        return resp.providers
    }

    /// SMART authorize URL for a tenant's FHIR base, to open in a browser.
    func authorizeUrl(fhirBaseUrl: String) async throws -> String? {
        let resp: EhrAuthorize = try await api.post(
            "/health/ehr/authorize",
            EhrAuthorizeRequest(fhirBaseUrl: fhirBaseUrl)
        )
        return resp.authorizeUrl
    }

    /// Pull Observations from the connected EHR; returns how many were stored.
    func sync() async throws -> Int {
        let resp: EhrSyncResult = try await api.post("/health/ehr/sync", EmptyBody())
        return resp.posted
    }
}

extension CharacterSet {
    /// URL query-value-safe set: `urlQueryAllowed` minus the sub-delimiters that a
    /// server may treat specially in a value (so `&`, `=`, `+`, etc. are escaped).
    static let urlQueryValueAllowed: CharacterSet = {
        var set = CharacterSet.urlQueryAllowed
        set.remove(charactersIn: "&=+?/;:@$,")
        return set
    }()
}

import Foundation
import Combine

/// Fetches and caches the server-side `/mirobody.json` document for the current
/// base URL. Mirrors `data/config/ServerConfigStore.kt`.
///
/// On every base-URL change it hydrates from the per-URL cache and kicks off a
/// background refresh. `config` is nil until something lands.
final class ServerConfigStore: ObservableObject {
    @Published private(set) var config: ServerConfig?

    private let api: ApiClient
    private let settings: SettingsStore
    private var cancellable: AnyCancellable?

    init(api: ApiClient, settings: SettingsStore) {
        self.api = api
        self.settings = settings
        // @Published replays the current value on subscribe, so this also runs once
        // immediately for the initial base URL.
        cancellable = settings.$baseURL
            .removeDuplicates()
            .sink { [weak self] url in
                guard let self else { return }
                self.config = self.loadCached(url)
                Task { try? await self.refreshInternal(url) }
            }
    }

    @discardableResult
    func refresh() async throws -> ServerConfig {
        try await refreshInternal(settings.baseURLValue)
    }

    private func loadCached(_ baseURL: String) -> ServerConfig? {
        guard let raw = settings.cachedServerConfig(baseURL: baseURL),
              let data = raw.data(using: .utf8) else { return nil }
        return try? JSONDecoder().decode(ServerConfig.self, from: data)
    }

    @discardableResult
    private func refreshInternal(_ baseURL: String) async throws -> ServerConfig {
        let fetched: ServerConfig = try await api.getRaw("/mirobody.json")
        if let data = try? JSONEncoder().encode(fetched),
           let json = String(data: data, encoding: .utf8) {
            settings.setCachedServerConfig(baseURL: baseURL, json: json)
        }
        await MainActor.run { self.config = fetched }
        return fetched
    }
}

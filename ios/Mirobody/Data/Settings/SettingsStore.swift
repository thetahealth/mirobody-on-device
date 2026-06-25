import Foundation
import Combine

/// Persisted app settings — the iOS analogue of `data/settings/SettingsStore.kt`.
/// Backed by `UserDefaults` (the DataStore equivalent). `@Published` properties
/// drive SwiftUI; the `*Value` accessors read `UserDefaults` directly so the API
/// client always sees the current base URL / token regardless of thread.
final class SettingsStore: ObservableObject {

    static let defaultBaseURL = "http://localhost:8080"
    private static let supported: Set<String> = ["zh", "ja", "ko", "en", "fr", "de", "ru", "es", "ar", "he"]

    @Published private(set) var baseURL: String
    @Published private(set) var accessToken: String?
    @Published private(set) var lastEmail: String?
    @Published private(set) var language: String
    @Published private(set) var selectedProviderName: String?
    @Published private(set) var fontSizeOffset: Int

    private let defaults: UserDefaults

    init(defaults: UserDefaults = .standard) {
        self.defaults = defaults
        self.baseURL = (defaults.string(forKey: Key.baseURL)?.nonBlank) ?? Self.defaultBaseURL
        self.accessToken = defaults.string(forKey: Key.accessToken)?.nonBlank
        self.lastEmail = defaults.string(forKey: Key.lastEmail)?.nonBlank
        self.language = defaults.string(forKey: Key.language) ?? Self.defaultLanguage()
        self.selectedProviderName = defaults.string(forKey: Key.selectedProvider)?.nonBlank
        self.fontSizeOffset = defaults.integer(forKey: Key.fontSizeOffset)
    }

    // MARK: Direct accessors for the API client (thread-safe, always current)

    var baseURLValue: String { (defaults.string(forKey: Key.baseURL)?.nonBlank) ?? Self.defaultBaseURL }
    var tokenValue: String? { defaults.string(forKey: Key.accessToken)?.nonBlank }

    // MARK: Setters

    func setBaseURL(_ value: String) {
        let trimmed = value.trimmingCharacters(in: .whitespaces)
        let normalized = trimmed.hasSuffix("/") ? String(trimmed.dropLast()) : trimmed
        defaults.set(normalized, forKey: Key.baseURL)
        publish { self.baseURL = normalized }
    }

    func setAccessToken(_ token: String?) {
        if let token, !token.isEmpty {
            defaults.set(token, forKey: Key.accessToken)
        } else {
            defaults.removeObject(forKey: Key.accessToken)
        }
        publish { self.accessToken = token?.nonBlank }
    }

    func setLastEmail(_ email: String?) {
        if let email, !email.isEmpty {
            defaults.set(email, forKey: Key.lastEmail)
        } else {
            defaults.removeObject(forKey: Key.lastEmail)
        }
        publish { self.lastEmail = email?.nonBlank }
    }

    func setLanguage(_ code: String) {
        defaults.set(code, forKey: Key.language)
        publish { self.language = code }
    }

    func setSelectedProviderName(_ name: String?) {
        if let name, !name.isEmpty {
            defaults.set(name, forKey: Key.selectedProvider)
        } else {
            defaults.removeObject(forKey: Key.selectedProvider)
        }
        publish { self.selectedProviderName = name?.nonBlank }
    }

    func setFontSizeOffset(_ offset: Int) {
        defaults.set(offset, forKey: Key.fontSizeOffset)
        publish { self.fontSizeOffset = offset }
    }

    /// Clears the token only — mirrors `clearAuth()`. The token watcher routes the
    /// UI back to login.
    func clearAuth() {
        defaults.removeObject(forKey: Key.accessToken)
        publish { self.accessToken = nil }
    }

    // MARK: Cached server config (per base URL)

    func cachedServerConfig(baseURL: String) -> String? {
        guard defaults.string(forKey: Key.cachedConfigURL) == baseURL else { return nil }
        return defaults.string(forKey: Key.cachedConfigJSON)
    }

    func setCachedServerConfig(baseURL: String, json: String) {
        defaults.set(baseURL, forKey: Key.cachedConfigURL)
        defaults.set(json, forKey: Key.cachedConfigJSON)
    }

    // MARK: Helpers

    static func defaultLanguage() -> String {
        let sys = Locale.current.language.languageCode?.identifier ?? "en"
        return supported.contains(sys) ? sys : "en"
    }

    /// `@Published` must mutate on the main thread; setters may be called from
    /// background async contexts (e.g. a 401 handler).
    private func publish(_ block: @escaping () -> Void) {
        if Thread.isMainThread { block() }
        else { DispatchQueue.main.async(execute: block) }
    }

    private enum Key {
        static let baseURL = "base_url"
        static let accessToken = "access_token"
        static let lastEmail = "last_email"
        static let language = "language"
        static let selectedProvider = "selected_provider_name"
        static let fontSizeOffset = "font_size_offset"
        static let cachedConfigURL = "cached_config_url"
        static let cachedConfigJSON = "cached_config_json"
    }
}

extension String {
    var nonBlank: String? { trimmingCharacters(in: .whitespaces).isEmpty ? nil : self }
}

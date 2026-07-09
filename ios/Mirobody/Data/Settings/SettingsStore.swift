import Foundation
import Combine

/// A stored account for the switcher: its opaque JWT `sub`, its email (may be
/// blank for a social login that returned none), and whether it's the active one.
/// Mirrors Android's `StoredAccount`.
struct StoredAccount: Identifiable, Equatable {
    let sub: String
    let email: String
    let isCurrent: Bool
    var id: String { sub }
}

/// Persisted app settings — the iOS analogue of `data/settings/SettingsStore.kt`.
/// Backed by `UserDefaults` (the DataStore equivalent). `@Published` properties
/// drive SwiftUI; the `*Value` accessors read `UserDefaults` directly so the API
/// client always sees the current base URL / token regardless of thread.
///
/// Multi-account: each account's JWT lives under `access_token_<sub>` and
/// `current_sub` names the active one, so several accounts coexist on one device.
/// `accessToken` / `tokenValue` expose the CURRENT account's token unchanged — the
/// API client and the router just see the active account.
final class SettingsStore: ObservableObject {

    static let defaultBaseURL = "http://localhost:8080"
    private static let supported: Set<String> = ["zh", "ja", "ko", "en", "fr", "de", "ru", "es", "ar", "he"]

    @Published private(set) var baseURL: String
    /// The CURRENT account's token (nil when signed out). Drives RootView routing.
    @Published private(set) var accessToken: String?
    /// The active account's `sub` ("" when signed out) — used to give ChatView a
    /// stable identity so switching accounts recreates it, and to namespace data.
    @Published private(set) var currentAccountId: String
    /// The active account's email (from the JWT `email` claim); nil when absent.
    @Published private(set) var currentEmail: String?
    /// Every stored account (current first) for the drawer's account switcher.
    @Published private(set) var accounts: [StoredAccount]
    /// True while "Add account" shows the login view over an existing session (a
    /// token is still stored, but we render login so a second account can sign in).
    /// In-memory only: a relaunch just returns to the current account.
    @Published var addingAccount = false
    @Published private(set) var lastEmail: String?
    @Published private(set) var language: String
    @Published private(set) var selectedProviderName: String?
    @Published private(set) var fontSizeOffset: Int

    private let defaults: UserDefaults

    init(defaults: UserDefaults = .standard) {
        self.defaults = defaults
        self.baseURL = (defaults.string(forKey: Key.baseURL)?.nonBlank) ?? Self.defaultBaseURL
        self.lastEmail = defaults.string(forKey: Key.lastEmail)?.nonBlank
        self.language = defaults.string(forKey: Key.language) ?? Self.defaultLanguage()
        self.selectedProviderName = defaults.string(forKey: Key.selectedProvider)?.nonBlank
        self.fontSizeOffset = defaults.integer(forKey: Key.fontSizeOffset)
        // Placeholders; filled by refreshAccounts() below (after legacy migration).
        self.accessToken = nil
        self.currentAccountId = ""
        self.currentEmail = nil
        self.accounts = []
        migrateLegacyIfNeeded()
        refreshAccounts()
    }

    // MARK: Direct accessors for the API client (thread-safe, always current)

    var baseURLValue: String { (defaults.string(forKey: Key.baseURL)?.nonBlank) ?? Self.defaultBaseURL }
    /// The current account's token, read straight from `UserDefaults` (any thread).
    var tokenValue: String? { currentToken() }

    // MARK: Setters

    func setBaseURL(_ value: String) {
        let trimmed = value.trimmingCharacters(in: .whitespaces)
        let normalized = trimmed.hasSuffix("/") ? String(trimmed.dropLast()) : trimmed
        defaults.set(normalized, forKey: Key.baseURL)
        publish { self.baseURL = normalized }
    }

    /// Store a token under its own account slot and make it current. Dedupes by
    /// email: the server re-salts `sub` per issuance, so the same account re-logging
    /// in gets a new sub — drop any existing slot with the same email first, so
    /// there's one entry per account rather than one per login. A blank token signs
    /// the current account out (mirrors `clearAuth`).
    func setAccessToken(_ token: String?) {
        guard let token, !token.trimmingCharacters(in: .whitespaces).isEmpty else {
            clearAuth()
            return
        }
        guard let sub = Self.claim("sub", of: token) else { return }
        let email = Self.claim("email", of: token)
        if let email {
            for key in tokenKeys() where key != Key.tokenPrefix + sub {
                if let other = defaults.string(forKey: key), Self.claim("email", of: other) == email {
                    defaults.removeObject(forKey: key)
                }
            }
        }
        defaults.set(token, forKey: Key.tokenPrefix + sub)
        defaults.set(sub, forKey: Key.currentSub)
        defaults.removeObject(forKey: Key.legacyToken)   // legacy slot no longer used
        publish {
            self.addingAccount = false
            self.refreshAccountsMainThread()
        }
    }

    /// Make an already-stored account current (no-op if its slot is gone).
    func switchAccount(_ sub: String) {
        guard defaults.string(forKey: Key.tokenPrefix + sub) != nil else { return }
        defaults.set(sub, forKey: Key.currentSub)
        publish {
            self.addingAccount = false
            self.refreshAccountsMainThread()
        }
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

    /// Sign out the CURRENT account: drop its slot, then fall back to another stored
    /// account if one exists (stay signed in as it), else clear the pointer (signed
    /// out → `accessToken` becomes nil → the router returns to login).
    func clearAuth() {
        if let sub = defaults.string(forKey: Key.currentSub) {
            defaults.removeObject(forKey: Key.tokenPrefix + sub)
        }
        if let next = tokenKeys().first {
            defaults.set(String(next.dropFirst(Key.tokenPrefix.count)), forKey: Key.currentSub)
        } else {
            defaults.removeObject(forKey: Key.currentSub)
        }
        defaults.removeObject(forKey: Key.legacyToken)
        publish {
            self.addingAccount = false
            self.refreshAccountsMainThread()
        }
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

    // MARK: Multi-account internals

    /// Keys of every stored per-account token slot ("access_token_<sub>").
    private func tokenKeys() -> [String] {
        defaults.dictionaryRepresentation().keys
            .filter { $0.hasPrefix(Key.tokenPrefix) }
            .sorted()
    }

    /// The current account's token, falling back to the legacy single-token slot for
    /// a pre-migration read.
    private func currentToken() -> String? {
        if let sub = defaults.string(forKey: Key.currentSub) {
            return defaults.string(forKey: Key.tokenPrefix + sub)?.nonBlank
        }
        return defaults.string(forKey: Key.legacyToken)?.nonBlank
    }

    /// Convert the old single-token slot into the per-account scheme, once.
    private func migrateLegacyIfNeeded() {
        guard let legacy = defaults.string(forKey: Key.legacyToken)?.nonBlank else { return }
        if defaults.string(forKey: Key.currentSub) == nil, let sub = Self.claim("sub", of: legacy) {
            defaults.set(legacy, forKey: Key.tokenPrefix + sub)
            defaults.set(sub, forKey: Key.currentSub)
        }
        defaults.removeObject(forKey: Key.legacyToken)
    }

    /// Recompute the published account state from `UserDefaults` (main-thread safe).
    private func refreshAccounts() {
        publish { self.refreshAccountsMainThread() }
    }

    /// Recompute the published account state; MUST run on the main thread.
    private func refreshAccountsMainThread() {
        let cur = defaults.string(forKey: Key.currentSub) ?? ""
        let token = currentToken()
        let list: [StoredAccount] = tokenKeys().map { key in
            let sub = String(key.dropFirst(Key.tokenPrefix.count))
            let email = defaults.string(forKey: key).flatMap { Self.claim("email", of: $0) } ?? ""
            return StoredAccount(sub: sub, email: email, isCurrent: sub == cur)
        }
        .sorted { ($0.isCurrent ? 0 : 1) < ($1.isCurrent ? 0 : 1) }
        self.accessToken = token
        self.currentAccountId = token != nil ? cur : ""
        self.currentEmail = token.flatMap { Self.claim("email", of: $0) }
        self.accounts = list
    }

    // MARK: JWT

    /// Decodes a JWT's payload (middle segment) and returns the requested string
    /// claim, or nil when the token is missing/malformed or the claim is absent.
    /// Base64url → UTF-8 JSON. Mirrors `SettingsStore.decodePayload` on Android.
    static func claim(_ name: String, of token: String) -> String? {
        let parts = token.split(separator: ".", omittingEmptySubsequences: false)
        guard parts.count >= 2 else { return nil }
        var b64 = String(parts[1]).replacingOccurrences(of: "-", with: "+")
            .replacingOccurrences(of: "_", with: "/")
        while b64.count % 4 != 0 { b64 += "=" }
        guard let data = Data(base64Encoded: b64),
              let obj = try? JSONSerialization.jsonObject(with: data) as? [String: Any],
              let value = obj[name] as? String,
              !value.isEmpty
        else { return nil }
        return value
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
        /// Legacy single-token slot, migrated to the per-account scheme on launch.
        static let legacyToken = "access_token"
        /// Per-account tokens live under "access_token_<sub>"; `currentSub` names the
        /// active account.
        static let tokenPrefix = "access_token_"
        static let currentSub = "current_sub"
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

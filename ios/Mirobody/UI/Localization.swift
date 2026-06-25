import SwiftUI

/// Runtime language override — the iOS analogue of `ui/LocaleProvider.kt`.
///
/// iOS normally resolves `Localizable.strings` against the system language, and
/// `Text(LocalizedStringKey)` ignores `\.locale`. To switch language in-app (as
/// Android does without an Activity recreate) we look strings up in the matching
/// `<lang>.lproj` bundle ourselves. The chosen language lives in the environment;
/// `LText` reads it, so changing it re-renders every label.

/// Right-to-left languages (Arabic, Hebrew). Because the app overrides the
/// language in-app rather than relying on the system locale, SwiftUI won't flip
/// the layout on its own — RootView feeds this into `\.layoutDirection`. Mirrors
/// the web client's RTL_LANGS and Android's RTL handling.
let rtlLanguages: Set<String> = ["ar", "he"]
func isRTL(_ language: String) -> Bool { rtlLanguages.contains(language) }

private struct MBLanguageKey: EnvironmentKey { static let defaultValue = "en" }
extension EnvironmentValues {
    var mbLanguage: String {
        get { self[MBLanguageKey.self] }
        set { self[MBLanguageKey.self] = newValue }
    }
}

/// Caches resolved `.lproj` bundles by language code (lookups happen on the main
/// thread during rendering).
private final class LanguageBundleCache {
    static let shared = LanguageBundleCache()
    private var cache: [String: Bundle] = [:]
    func bundle(for language: String) -> Bundle {
        if let b = cache[language] { return b }
        let b = Bundle.main.path(forResource: language, ofType: "lproj")
            .flatMap(Bundle.init(path:)) ?? .main
        cache[language] = b
        return b
    }
}

/// Looks up `key` in the given language, applying printf-style arguments.
func L(_ key: String, _ language: String, arguments: [CVarArg]) -> String {
    let format = LanguageBundleCache.shared.bundle(for: language)
        .localizedString(forKey: key, value: key, table: nil)
    guard !arguments.isEmpty else { return format }
    return String(format: format, locale: Locale(identifier: language), arguments: arguments)
}

func L(_ key: String, _ language: String, _ arguments: CVarArg...) -> String {
    L(key, language, arguments: arguments)
}

/// A `Text` that localizes against the environment's `mbLanguage`.
struct LText: View {
    @Environment(\.mbLanguage) private var language
    private let key: String
    private let arguments: [CVarArg]

    init(_ key: String, _ arguments: CVarArg...) {
        self.key = key
        self.arguments = arguments
    }

    var body: Text {
        Text(verbatim: L(key, language, arguments: arguments))
    }
}

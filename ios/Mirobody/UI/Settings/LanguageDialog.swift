import SwiftUI

/// Supported languages, in the same order as `ui/settings/LanguageDialog.kt`.
let languageOptions: [(code: String, label: String)] = [
    ("zh", "中文"),
    ("ja", "日本語"),
    ("ko", "한국어"),
    ("en", "English"),
    ("fr", "Français"),
    ("de", "Deutsch"),
    ("ru", "Русский"),
    ("es", "Español"),
    ("ar", "العربية"),
    // Hebrew is "iw" on Android (Java normalizes "he" -> "iw"); iOS/Apple uses
    // "he", which is also the .lproj folder name (he.lproj).
    ("he", "עברית"),
]

/// Language picker — mirrors `LanguageDialog`. Android uses a wheel picker; the
/// iOS `.wheel` picker style is the natural equivalent. Chrome is localized in the
/// currently-active language (the Android dialog wraps slots in `ProvideLocale`).
struct LanguageDialog: View {
    let current: String
    let onPick: (String) -> Void

    @Environment(\.mbColors) private var colors
    @Environment(\.dismiss) private var dismiss
    @State private var staged: String

    init(current: String, onPick: @escaping (String) -> Void) {
        self.current = current
        self.onPick = onPick
        _staged = State(initialValue: current)
    }

    var body: some View {
        VStack(spacing: 16) {
            Text(L("chat_language", current)).mbFont(.titleMedium).foregroundColor(colors.onSurface)
            Picker("", selection: $staged) {
                ForEach(languageOptions, id: \.code) { option in
                    Text(option.label).tag(option.code)
                }
            }
            .pickerStyle(.wheel)
            HStack {
                Button(L("common_cancel", current)) { dismiss() }
                Spacer()
                Button(L("common_done", current)) { onPick(staged); dismiss() }
            }
            .foregroundColor(colors.primary)
        }
        .padding(24)
        .presentationDetents([.height(300)])
    }
}

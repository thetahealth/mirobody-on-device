import SwiftUI

/// The five font tiers, persisted point offset -> label key. Top-level so both the
/// picker and the drawer row that opens it (which shows the current tier as its hint)
/// read the same mapping.
let fontSizeTiers: [(offset: Int, labelKey: String)] = [
    (-4, "chat_font_size_smaller"),
    (-2, "chat_font_size_small"),
    (0, "chat_font_size_normal"),
    (2, "chat_font_size_large"),
    (4, "chat_font_size_larger"),
]

/// Label for a stored offset; anything unrecognized reads as the normal tier.
func fontTierLabelKey(forOffset offset: Int) -> String {
    fontSizeTiers.first { $0.offset == offset }?.labelKey ?? "chat_font_size_normal"
}

/// The app-settings group of the nav drawer: Language, Font size, Backend — each a
/// drawer row carrying its current value as a trailing hint, and each opening the
/// dialog that changes it.
///
/// This used to be a top-right gear on both the chat and auth screens. It moved into
/// the drawer to follow the web client (`htdoc/src/history.js`), which folded the same
/// menu in so every screen has exactly one menu affordance instead of a hamburger and
/// a gear competing for the same job. Signed out, this group IS the whole drawer —
/// everything else in it is session-scoped.
struct AppSettingsSection: View {
    @EnvironmentObject private var settings: SettingsStore
    @Environment(\.mbLanguage) private var lang

    @State private var showLanguage = false
    @State private var showFontSize = false
    @State private var showBackend = false

    var body: some View {
        VStack(spacing: 0) {
            DrawerRow(titleKey: "chat_language", hint: languageHint) { showLanguage = true }
            DrawerRow(titleKey: "chat_font_size", hint: fontSizeHint) { showFontSize = true }
            DrawerRow(titleKey: "chat_backend", hint: backendHint) { showBackend = true }
        }
        .sheet(isPresented: $showLanguage) {
            sheetEnv { LanguageDialog(current: settings.language) { settings.setLanguage($0) } }
        }
        .sheet(isPresented: $showFontSize) { sheetEnv { FontSizeDialog() } }
        .sheet(isPresented: $showBackend) { sheetEnv { BaseUrlDialog() } }
    }

    /// The language's own endonym, as the picker lists it — a row reading
    /// "Language  English" is answerable without opening it.
    private var languageHint: String {
        languageOptions.first { $0.code == settings.language }?.label ?? settings.language
    }

    private var fontSizeHint: String {
        L(fontTierLabelKey(forOffset: settings.fontSizeOffset), lang)
    }

    /// Host only — the full address (scheme, port, path) belongs in the dialog this
    /// row opens, not in a one-line hint that would truncate away the identifying part.
    private var backendHint: String {
        URLComponents(string: settings.baseURL)?.host ?? ""
    }

    /// Re-injects the app environment onto presented sheets (mirrors ChatView's helper).
    @ViewBuilder
    private func sheetEnv<Content: View>(@ViewBuilder _ content: () -> Content) -> some View {
        content()
            .environmentObject(settings)
            .environment(\.mbLanguage, lang)
            .environment(\.mbFontScale, fontScale(forOffset: settings.fontSizeOffset))
    }
}

/// The auth screens' drawer: the header and the app-settings group, nothing else.
/// History, New chat / Incognito, health connections and the account rows are all
/// session-scoped, so signed out there is nothing else to show — and no scrolling band
/// either, which is why the group sits straight under the header rather than being
/// pushed to the bottom of a blank panel by a spacer.
struct SettingsDrawer: View {
    let onDismiss: () -> Void

    @Environment(\.mbColors) private var colors

    var body: some View {
        VStack(spacing: 0) {
            DrawerHeader(onDismiss: onDismiss)
            Divider().background(colors.outlineVariant.opacity(0.5))
            AppSettingsSection()
            Spacer(minLength: 0)
        }
        .background(colors.background)
    }
}

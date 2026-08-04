import SwiftUI

/// Backend presets — mirrors `ui/settings/BaseUrlPresets.kt`.
let baseURLPresets: [String] = [
    SettingsStore.defaultBaseURL,
    "https://test.mirobody.ai",
]

/// "Backend" dialog — mirrors `ui/settings/BaseUrlDialog.kt`. Presented as a sheet.
struct BaseUrlDialog: View {
    @EnvironmentObject private var settings: SettingsStore
    @Environment(\.mbColors) private var colors
    @Environment(\.mbLanguage) private var lang
    @Environment(\.dismiss) private var dismiss

    @State private var input: String = ""
    @State private var error: String?

    var body: some View {
        VStack(alignment: .leading, spacing: 12) {
            LText("baseurl_title").mbFont(.titleMedium).foregroundColor(colors.onSurface)
            LText("baseurl_subtitle").mbFont(.bodyMedium).foregroundColor(colors.onSurfaceVariant)

            HStack(spacing: 8) {
                TextField("http://10.0.2.2:18080", text: $input)
                    .textInputAutocapitalization(.never)
                    .autocorrectionDisabled()
                    .keyboardType(.URL)
                    .mbFont(.bodyLarge)
                    .foregroundColor(colors.onSurface)
                    .padding(12)
                    .overlay(RoundedRectangle(cornerRadius: 10)
                        .stroke(error != nil ? colors.error : colors.outlineVariant, lineWidth: 1))
                Menu {
                    ForEach(baseURLPresets, id: \.self) { preset in
                        Button(preset) { input = preset; error = nil }
                    }
                } label: {
                    Image(systemName: "chevron.down").foregroundColor(colors.onSurfaceVariant).padding(8)
                }
            }
            if let error {
                Text(error).mbFont(.bodySmall).foregroundColor(colors.error)
            }
            LText("baseurl_hint").mbFont(.bodySmall).foregroundColor(colors.onSurfaceVariant)

            HStack {
                Button(L("common_cancel", lang)) { dismiss() }
                Spacer()
                Button(L("common_save", lang)) { save() }
            }
            .foregroundColor(colors.primary)
            Spacer()
        }
        .padding(24)
        .presentationDetents([.medium])
        .onAppear { if input.isEmpty { input = settings.baseURL } }
    }

    private func save() {
        var trimmed = input.trimmingCharacters(in: .whitespaces)
        if trimmed.hasSuffix("/") { trimmed = String(trimmed.dropLast()) }
        guard isValidHTTPURL(trimmed) else {
            error = L("baseurl_invalid", lang)
            return
        }
        settings.setBaseURL(trimmed)
        dismiss()
    }

    private func isValidHTTPURL(_ s: String) -> Bool {
        guard let comps = URLComponents(string: s),
              let scheme = comps.scheme?.lowercased(),
              scheme == "http" || scheme == "https",
              let host = comps.host, !host.isEmpty else { return false }
        return true
    }
}

// The pre-auth settings gear is gone: the auth screens open the same nav drawer the
// chat screen does, narrowed to the app-settings group. See `AppSettingsSection` and
// `SettingsDrawer` in UI/Settings/AppSettingsSection.swift.

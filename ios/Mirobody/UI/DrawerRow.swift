import SwiftUI

/// Width of the sliding nav drawer: 85% of the screen, capped at `drawerMaxWidth`
/// (mirrors the web and Android drawers). Top-level because two drawers use it now —
/// the chat one and the login screen's settings-only one.
var mbDrawerWidth: CGFloat {
    min(UIScreen.main.bounds.width * 0.85, drawerMaxWidth)
}

/// One tappable row in the nav drawer: the label, and an optional right-aligned hint
/// carrying the row's current value (the chosen language, the backend host).
///
/// Shared rather than private to `NavDrawer` because the app-settings group renders
/// the same row from both drawers.
struct DrawerRow: View {
    let titleKey: String
    var hint: String? = nil
    let action: () -> Void

    @Environment(\.mbColors) private var colors

    var body: some View {
        Button(action: action) {
            HStack(spacing: 12) {
                LText(titleKey).mbFont(.bodyLarge).foregroundColor(colors.onSurface)
                Spacer(minLength: 12)
                if let hint, !hint.isEmpty {
                    // The hint takes the remaining width and truncates — a backend
                    // address can be a long host name.
                    Text(hint)
                        .mbFont(.bodySmall)
                        .foregroundColor(colors.onSurfaceVariant)
                        .lineLimit(1)
                        .truncationMode(.tail)
                }
            }
            .frame(maxWidth: .infinity, alignment: .leading)
            .padding(.horizontal, 20).padding(.vertical, 12)
            .contentShape(Rectangle())
        }
    }
}

/// The drawer's header: a back button that closes it, then the "Menu" title. Shared
/// so the chat drawer and the login screen's settings-only drawer open into the same
/// frame. `chevron.backward` is direction-aware, so it points the right way in RTL.
struct DrawerHeader: View {
    let onDismiss: () -> Void

    @Environment(\.mbColors) private var colors
    @Environment(\.mbLanguage) private var lang

    var body: some View {
        HStack(spacing: 4) {
            Button { onDismiss() } label: {
                Image(systemName: "chevron.backward").foregroundColor(colors.onSurfaceVariant)
            }
            .frame(width: 40, height: 40)
            .accessibilityLabel(L("common_back", lang))
            LText("chat_menu_title").mbFont(.titleMedium).foregroundColor(colors.onSurface)
            Spacer()
        }
        .padding(.horizontal, 8).padding(.top, 4)
        .frame(height: 56)
    }
}

/// The top bar's drawer button, on every screen that has a drawer to open. A nav
/// glyph rather than the account avatar this used to be on chat: the drawer's body is
/// the conversation list and account is one pinned row at the bottom, so a profile
/// icon undersold what the button opens.
struct DrawerMenuButton: View {
    let action: () -> Void

    @Environment(\.mbColors) private var colors
    @Environment(\.mbLanguage) private var lang

    var body: some View {
        Button(action: action) {
            Image(systemName: "line.3.horizontal").foregroundColor(colors.onSurfaceVariant)
        }
        .accessibilityLabel(L("chat_menu_title", lang))
    }
}

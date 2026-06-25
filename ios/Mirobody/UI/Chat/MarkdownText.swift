import SwiftUI
import MarkdownUI

/// Renders chat content as GFM markdown — the iOS counterpart to `MarkdownText.kt`
/// (which uses Markwon on Android). Tables, code, strikethrough, and auto-links are
/// supported via MarkdownUI.
///
/// Known gap vs. Android: inline/block LaTeX (`$…$` / `$$…$$`) is not rendered.
/// Markwon's JLatexMath plugin has no drop-in SwiftUI equivalent; add one here if
/// math rendering becomes a requirement.
struct MarkdownText: View {
    let text: String
    var textStyle: MBTextStyle = .bodyMedium

    @Environment(\.mbFontScale) private var scale
    @Environment(\.mbColors) private var colors

    var body: some View {
        Markdown(text)
            .markdownTextStyle {
                FontSize(textStyle.size * scale)
                ForegroundColor(colors.onSurface)
            }
            .tint(colors.primary)   // link / accent color
            .textSelection(.enabled)
    }
}

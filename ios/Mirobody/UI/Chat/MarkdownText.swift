import SwiftUI
import MarkdownUI

/// Renders chat content as GFM markdown — the iOS counterpart to `MarkdownText.kt`
/// (which uses Markwon on Android). Tables, code, strikethrough, and auto-links are
/// supported via MarkdownUI.
///
/// Math: MarkdownUI has no KaTeX equivalent, so messages that contain `$…$` / `$$…$$`
/// are rendered instead by `MathMarkdownText` (an offline KaTeX WebView), matching
/// Android's JLatexMath and the web client. That path is used only once a turn has
/// settled (`streaming == false`) — while tokens stream in we keep the fast native
/// renderer and show math as source, then swap to rendered math when the turn ends.
struct MarkdownText: View {
    let text: String
    var textStyle: MBTextStyle = .bodyMedium
    /// Overrides the body text color (e.g. white on the solid-navy user bubble).
    var color: Color? = nil
    /// Suppresses the KaTeX path mid-stream (see the note above).
    var streaming: Bool = false

    @Environment(\.mbFontScale) private var scale
    @Environment(\.mbColors) private var colors

    var body: some View {
        if !streaming && Self.containsMath(text) {
            MathMarkdownText(
                text: text,
                fontSize: textStyle.size * scale,
                textColor: color ?? colors.onSurface,
                linkColor: color ?? colors.primary
            )
        } else {
            Markdown(text)
                .markdownTextStyle {
                    FontSize(textStyle.size * scale)
                    ForegroundColor(color ?? colors.onSurface)
                }
                .tint(color ?? colors.primary)   // link / accent color
                .textSelection(.enabled)
        }
    }

    /// Detects `$$…$$` block or `$…$` inline math using the same guards as the web
    /// client (`htdoc/src/markdown.js`), so currency like "$5 and $6" isn't matched.
    static func containsMath(_ s: String) -> Bool {
        guard s.contains("$") else { return false }
        if s.range(of: #"\$\$[\s\S]+?\$\$"#, options: .regularExpression) != nil { return true }
        if s.range(of: #"\$(?![\s$])[^\n$]*?[^\s$]\$(?!\d)"#, options: .regularExpression) != nil { return true }
        return false
    }
}

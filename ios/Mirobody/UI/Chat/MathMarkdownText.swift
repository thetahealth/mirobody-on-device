import SwiftUI
import WebKit

/// Renders chat markdown that contains LaTeX math in a self-sizing, offline
/// `WKWebView` running the same marked + KaTeX + DOMPurify pipeline as the web
/// client (`htdoc/src/markdown.js`). This is the iOS answer to the "no math"
/// gap in `MarkdownText` — MarkdownUI has no KaTeX equivalent, so math-bearing
/// messages take this path while plain messages stay on native MarkdownUI.
///
/// Assets live under `math/` (a folder reference) so the page, KaTeX JS/CSS and
/// its woff2 fonts all load from `file://` with no network access — mirroring how
/// `EChartsView` bundles `echarts.min.js`.
struct MathMarkdownText: View {
    let text: String
    let fontSize: CGFloat
    let textColor: Color
    let linkColor: Color

    @State private var height: CGFloat = 1

    var body: some View {
        MathWebView(
            text: text, fontSize: fontSize,
            textColor: textColor, linkColor: linkColor, height: $height
        )
        .frame(height: height)
        .frame(maxWidth: .infinity, alignment: .leading)
    }
}

private struct MathWebView: UIViewRepresentable {
    let text: String
    let fontSize: CGFloat
    let textColor: Color
    let linkColor: Color
    @Binding var height: CGFloat

    func makeCoordinator() -> Coordinator { Coordinator(height: $height) }

    func makeUIView(context: Context) -> WKWebView {
        let cfg = WKWebViewConfiguration()
        cfg.userContentController.add(context.coordinator, name: "resize")
        let web = WKWebView(frame: .zero, configuration: cfg)
        web.navigationDelegate = context.coordinator
        web.isOpaque = false
        web.backgroundColor = .clear
        web.scrollView.backgroundColor = .clear
        web.scrollView.isScrollEnabled = false
        web.scrollView.bounces = false
        if let url = Self.pageURL {
            web.loadFileURL(url, allowingReadAccessTo: url.deletingLastPathComponent())
        }
        return web
    }

    func updateUIView(_ web: WKWebView, context: Context) {
        // Re-render only when an input actually changed, so unrelated SwiftUI redraws
        // don't reload the page.
        let key = "\(Int(fontSize))|\(textColor.cssRGBA)|\(linkColor.cssRGBA)|\(text)"
        guard context.coordinator.lastKey != key else { return }
        context.coordinator.lastKey = key
        context.coordinator.pending = Payload(
            md: text, size: fontSize, fg: textColor.cssRGBA, link: linkColor.cssRGBA
        )
        if context.coordinator.loaded { context.coordinator.flush(web) }
    }

    private static let pageURL: URL? =
        Bundle.main.url(forResource: "render", withExtension: "html", subdirectory: "math")

    struct Payload { let md: String; let size: CGFloat; let fg: String; let link: String }

    final class Coordinator: NSObject, WKNavigationDelegate, WKScriptMessageHandler {
        private let height: Binding<CGFloat>
        var lastKey: String?
        var loaded = false
        var pending: Payload?

        init(height: Binding<CGFloat>) { self.height = height }

        func webView(_ web: WKWebView, didFinish navigation: WKNavigation!) {
            loaded = true
            flush(web)
        }

        func flush(_ web: WKWebView) {
            guard let p = pending else { return }
            let theme = "window.mbApplyTheme(\(Int(p.size)), '\(p.fg)', '\(p.link)');"
            let render = "window.mbRender(\(jsStringLiteral(p.md)));"
            web.evaluateJavaScript(theme + render, completionHandler: nil)
        }

        func userContentController(_ c: WKUserContentController, didReceive msg: WKScriptMessage) {
            guard msg.name == "resize", let n = msg.body as? NSNumber else { return }
            let v = max(CGFloat(truncating: n), 1)
            if abs(v - height.wrappedValue) > 0.5 { height.wrappedValue = v }
        }
    }
}

/// Encodes a Swift string as a JavaScript string literal (quotes included) so it can
/// be safely spliced into an `evaluateJavaScript` call.
private func jsStringLiteral(_ s: String) -> String {
    // JSONSerialization needs a container; encode `[s]` then strip the brackets.
    guard let data = try? JSONSerialization.data(withJSONObject: [s]),
          let json = String(data: data, encoding: .utf8) else { return "\"\"" }
    return String(json.dropFirst().dropLast())
}

private extension Color {
    /// A CSS `rgba(...)` string. MBColors are all sRGB (`Color(rgb:)`), so the
    /// component read is reliable.
    var cssRGBA: String {
        let ui = UIColor(self)
        var r: CGFloat = 0, g: CGFloat = 0, b: CGFloat = 0, a: CGFloat = 0
        ui.getRed(&r, green: &g, blue: &b, alpha: &a)
        return "rgba(\(Int(r * 255)),\(Int(g * 255)),\(Int(b * 255)),\(a))"
    }
}

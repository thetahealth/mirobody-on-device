import SwiftUI
import WebKit

/// Decides how to render an `image` SSE chunk — mirrors the `chatImageModel` helper
/// in `ImageViewerDialog.kt`. Raw SVG/XML is rendered in a `WKWebView` (SwiftUI's
/// `AsyncImage` can't decode SVG); everything else (http(s), `data:` URIs) goes
/// through `AsyncImage`.
func isRawSVG(_ content: String) -> Bool {
    let t = content.trimmingCharacters(in: .whitespacesAndNewlines)
    return t.hasPrefix("<svg") || t.hasPrefix("<?xml")
}

/// Inline chat image. `contentMode: .fit` fills the available width.
struct ChatImage: View {
    let content: String

    var body: some View {
        if isRawSVG(content) {
            SVGWebView(svg: content)
                .aspectRatio(16.0 / 9.0, contentMode: .fit)
        } else if let url = URL(string: content) {
            AsyncImage(url: url) { phase in
                switch phase {
                case .success(let image):
                    image.resizable().scaledToFit()
                case .failure:
                    Color.clear.frame(height: 1)
                case .empty:
                    ProgressView().frame(maxWidth: .infinity, minHeight: 60)
                @unknown default:
                    Color.clear.frame(height: 1)
                }
            }
        } else {
            Color.clear.frame(height: 1)
        }
    }
}

/// Renders a raw SVG document inside a transparent, non-scrolling web view.
struct SVGWebView: UIViewRepresentable {
    let svg: String

    func makeUIView(context: Context) -> WKWebView {
        let webView = WKWebView()
        webView.isOpaque = false
        webView.backgroundColor = .clear
        webView.scrollView.isScrollEnabled = false
        webView.scrollView.backgroundColor = .clear
        return webView
    }

    func updateUIView(_ webView: WKWebView, context: Context) {
        let html = """
        <!doctype html><html><head>
        <meta name="viewport" content="width=device-width, initial-scale=1">
        <style>html,body{margin:0;padding:0;background:transparent}
        svg,img{max-width:100%;height:auto;display:block}</style>
        </head><body>\(svg)</body></html>
        """
        webView.loadHTMLString(html, baseURL: nil)
    }
}

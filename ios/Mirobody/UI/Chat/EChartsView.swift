import SwiftUI
import WebKit

/// Renders an Apache ECharts chart from a backend `chart` SSE event.
///
/// `option` is the ECharts `option` as a JSON object string (see the backend's
/// `render_chart` tool). The echarts library is bundled (`echarts.min.js`) and
/// inlined into the page, so the WebView never touches the network.
///
/// The WebView is transparent, so the chart sits directly on the app's surface and
/// must follow the color scheme: echarts' built-in default paints axis labels and
/// titles near-black, unreadable on the dark surface. The chrome colors come from
/// the live `MBColors` rather than a copy of the palette in JS, so the chart can
/// never drift from the app around it (`chart-theme.js`).
struct EChartsView: View {
    let option: String

    @Environment(\.colorScheme) private var colorScheme
    @Environment(\.mbColors) private var colors

    var body: some View {
        EChartsWebView(
            option: option,
            dark: colorScheme == .dark,
            ink: colors.onSurface.cssHex,
            inkDim: colors.onSurfaceVariant.cssHex,
            axis: colors.outline.cssHex,
            grid: colors.outlineVariant.cssHex
        )
        .aspectRatio(16.0 / 9.0, contentMode: .fit)
    }
}

/// Hosts a transparent, non-scrolling `WKWebView` that runs echarts on `option`.
private struct EChartsWebView: UIViewRepresentable {
    let option: String
    let dark: Bool
    let ink: String
    let inkDim: String
    let axis: String
    let grid: String

    func makeUIView(context: Context) -> WKWebView {
        let webView = WKWebView()
        webView.isOpaque = false
        webView.backgroundColor = .clear
        webView.scrollView.isScrollEnabled = false
        webView.scrollView.backgroundColor = .clear
        return webView
    }

    func updateUIView(_ webView: WKWebView, context: Context) {
        // Charts are appended once per turn, so reload only when the rendered page
        // changes. It depends on the scheme as well as the option — keying on the
        // option alone would leave a chart in the old scheme after a theme flip.
        let page = html()
        guard context.coordinator.loadedPage != page else { return }
        context.coordinator.loadedPage = page
        webView.loadHTMLString(page, baseURL: nil)
    }

    func makeCoordinator() -> Coordinator { Coordinator() }
    final class Coordinator { var loadedPage: String? }

    /// Bundled scripts, read once. Inlined into the page so no file/network access
    /// is needed (which `loadHTMLString` with a nil baseURL would otherwise restrict).
    private static func bundledJS(_ name: String) -> String {
        guard let url = Bundle.main.url(forResource: name, withExtension: "js"),
              let js = try? String(contentsOf: url, encoding: .utf8) else { return "" }
        return js
    }
    private static let echartsJS: String = bundledJS("echarts.min")
    private static let themeJS: String = bundledJS("chart-theme")

    private func html() -> String {
        """
        <!doctype html><html><head>
        <meta name="viewport" content="width=device-width, initial-scale=1, maximum-scale=1, user-scalable=no">
        <style>html,body,#c{margin:0;padding:0;width:100%;height:100%;background:transparent}</style>
        <script>\(Self.echartsJS)</script>
        <script>\(Self.themeJS)</script>
        </head><body>
        <div id="c"></div>
        <script>
        (function(){
          var el = document.getElementById('c');
          // 'transparent' as the surface: this is a live canvas over the app's own
          // background, so fills need no opaque gap color to blend against.
          var theme = mbChartTheme(\(dark), {
            ink: '\(ink)', inkDim: '\(inkDim)', axis: '\(axis)', grid: '\(grid)',
            surface: 'transparent'
          });
          var chart = echarts.init(el, theme, { renderer: 'canvas' });
          try { chart.setOption(mbPrepare(\(option))); }
          catch (e) { el.innerHTML = '<pre style="color:\(ink);white-space:pre-wrap">' + e + '</pre>'; }
          window.addEventListener('resize', function(){ chart.resize(); });
        })();
        </script>
        </body></html>
        """
    }
}

private extension Color {
    /// `#rrggbb` for CSS. MBColors are all opaque sRGB (`Color(rgb:)`).
    var cssHex: String {
        var r: CGFloat = 0, g: CGFloat = 0, b: CGFloat = 0, a: CGFloat = 0
        UIColor(self).getRed(&r, green: &g, blue: &b, alpha: &a)
        return String(format: "#%02X%02X%02X",
                      Int((r * 255).rounded()), Int((g * 255).rounded()), Int((b * 255).rounded()))
    }
}

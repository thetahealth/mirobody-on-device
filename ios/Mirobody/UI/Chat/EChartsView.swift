import SwiftUI
import WebKit

/// Renders an Apache ECharts chart from a backend `chart` SSE event.
///
/// `option` is the ECharts `option` as a JSON object string (see the backend's
/// `render_chart` tool). The echarts library is bundled (`echarts.min.js`) and
/// inlined into the page, so the WebView never touches the network.
struct EChartsView: View {
    let option: String

    var body: some View {
        EChartsWebView(option: option)
            .aspectRatio(16.0 / 9.0, contentMode: .fit)
    }
}

/// Hosts a transparent, non-scrolling `WKWebView` that runs echarts on `option`.
private struct EChartsWebView: UIViewRepresentable {
    let option: String

    func makeUIView(context: Context) -> WKWebView {
        let webView = WKWebView()
        webView.isOpaque = false
        webView.backgroundColor = .clear
        webView.scrollView.isScrollEnabled = false
        webView.scrollView.backgroundColor = .clear
        return webView
    }

    func updateUIView(_ webView: WKWebView, context: Context) {
        // Charts are appended once per turn, so reload only when the option changes.
        guard context.coordinator.loadedOption != option else { return }
        context.coordinator.loadedOption = option
        webView.loadHTMLString(Self.html(option: option), baseURL: nil)
    }

    func makeCoordinator() -> Coordinator { Coordinator() }
    final class Coordinator { var loadedOption: String? }

    /// The bundled library, read once. Inlined into the page so no file/network
    /// access is needed (which `loadHTMLString` would otherwise restrict).
    private static let echartsJS: String = {
        guard let url = Bundle.main.url(forResource: "echarts.min", withExtension: "js"),
              let js = try? String(contentsOf: url, encoding: .utf8) else { return "" }
        return js
    }()

    private static func html(option: String) -> String {
        """
        <!doctype html><html><head>
        <meta name="viewport" content="width=device-width, initial-scale=1, maximum-scale=1, user-scalable=no">
        <style>html,body,#c{margin:0;padding:0;width:100%;height:100%;background:transparent}</style>
        <script>\(echartsJS)</script>
        </head><body>
        <div id="c"></div>
        <script>
        (function(){
          var el = document.getElementById('c');
          var chart = echarts.init(el, null, { renderer: 'canvas' });
          try { chart.setOption(\(option)); }
          catch (e) { el.innerHTML = '<pre style="color:#b00;white-space:pre-wrap">' + e + '</pre>'; }
          window.addEventListener('resize', function(){ chart.resize(); });
        })();
        </script>
        </body></html>
        """
    }
}

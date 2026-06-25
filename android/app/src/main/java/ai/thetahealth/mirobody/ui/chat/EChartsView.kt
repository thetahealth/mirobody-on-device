package ai.thetahealth.mirobody.ui.chat

import android.annotation.SuppressLint
import android.graphics.Color as AndroidColor
import android.webkit.WebView
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.runtime.Composable
import androidx.compose.ui.Modifier
import androidx.compose.ui.unit.dp
import androidx.compose.ui.viewinterop.AndroidView

/**
 * Renders an Apache ECharts chart from a backend `chart` SSE event.
 *
 * [optionJson] is the ECharts `option` as a JSON object string (see the backend's
 * `render_chart` tool). It is dropped into a tiny HTML page that loads the
 * echarts library bundled in `assets/echarts.min.js` and calls `setOption`.
 *
 * Everything the WebView loads is local and trusted (the bundled library plus our
 * own backend's option), so file access from the `file:///android_asset/` base is
 * safe — no remote content is ever fetched.
 */
@SuppressLint("SetJavaScriptEnabled")
@Suppress("DEPRECATION")   // allowFileAccessFromFileURLs: needed to load the bundled lib from file://
@Composable
fun EChartsView(optionJson: String, modifier: Modifier = Modifier) {
    AndroidView(
        modifier = modifier
            .fillMaxWidth()
            // ECharts needs an explicit container height; this matches the
            // ~16:9 inline feel of the existing chat image block.
            .height(240.dp),
        factory = { context ->
            WebView(context).apply {
                setBackgroundColor(AndroidColor.TRANSPARENT)
                settings.javaScriptEnabled = true
                settings.allowFileAccess = true
                settings.allowFileAccessFromFileURLs = true
            }
        },
        update = { webView ->
            // Reload only when the option actually changes (charts are appended
            // once per turn, so this is normally a no-op after the first load).
            if (webView.tag != optionJson) {
                webView.tag = optionJson
                webView.loadDataWithBaseURL(
                    "file:///android_asset/",
                    chartHtml(optionJson),
                    "text/html",
                    "utf-8",
                    null,
                )
            }
        },
    )
}

private fun chartHtml(optionJson: String): String = """
    <!doctype html>
    <html>
    <head>
      <meta name="viewport" content="width=device-width, initial-scale=1, maximum-scale=1, user-scalable=no">
      <style>
        html, body, #c { margin: 0; padding: 0; width: 100%; height: 100%; background: transparent; }
      </style>
      <script src="echarts.min.js"></script>
    </head>
    <body>
      <div id="c"></div>
      <script>
        (function () {
          var el = document.getElementById('c');
          var chart = echarts.init(el, null, { renderer: 'canvas' });
          try {
            chart.setOption($optionJson);
          } catch (e) {
            el.innerHTML = '<pre style="color:#b00;white-space:pre-wrap">' + e + '</pre>';
          }
          window.addEventListener('resize', function () { chart.resize(); });
        })();
      </script>
    </body>
    </html>
""".trimIndent()

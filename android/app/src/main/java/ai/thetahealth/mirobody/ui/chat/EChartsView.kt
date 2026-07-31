package ai.thetahealth.mirobody.ui.chat

import android.annotation.SuppressLint
import android.graphics.Color as AndroidColor
import android.webkit.WebView
import androidx.compose.foundation.isSystemInDarkTheme
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.material3.MaterialTheme
import androidx.compose.runtime.Composable
import androidx.compose.ui.Modifier
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.graphics.toArgb
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
 *
 * The WebView is transparent, so the chart sits directly on the app's surface and
 * MUST follow the app's color scheme: echarts' built-in default paints axis labels
 * and titles near-black, which is unreadable on the dark surface. The chrome colors
 * are handed to the page from the live MaterialTheme rather than mirrored in JS, so
 * the chart can never drift from the app around it (assets/chart-theme.js).
 */
@SuppressLint("SetJavaScriptEnabled")
@Suppress("DEPRECATION")   // allowFileAccessFromFileURLs: needed to load the bundled lib from file://
@Composable
fun EChartsView(optionJson: String, modifier: Modifier = Modifier) {
    val dark = isSystemInDarkTheme()
    val scheme = MaterialTheme.colorScheme
    val ink = scheme.onSurface.css()
    val inkDim = scheme.onSurfaceVariant.css()
    val axis = scheme.outline.css()
    val grid = scheme.outlineVariant.css()
    // The page is what must be reloaded, and it now depends on the scheme as well as
    // the option — keying the guard on the option alone would leave a chart rendered
    // in the old scheme after the user flips the system theme.
    val html = chartHtml(optionJson, dark, ink, inkDim, axis, grid)

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
            // Reload only when the rendered page actually changes (charts are appended
            // once per turn, so this is normally a no-op after the first load).
            if (webView.tag != html) {
                webView.tag = html
                webView.loadDataWithBaseURL(
                    "file:///android_asset/",
                    html,
                    "text/html",
                    "utf-8",
                    null,
                )
            }
        },
    )
}

/** `#rrggbb` for CSS. Alpha is dropped: every chrome color in the scheme is opaque. */
private fun Color.css(): String = String.format("#%06X", toArgb() and 0xFFFFFF)

private fun chartHtml(
    optionJson: String,
    dark: Boolean,
    ink: String,
    inkDim: String,
    axis: String,
    grid: String,
): String = """
    <!doctype html>
    <html>
    <head>
      <meta name="viewport" content="width=device-width, initial-scale=1, maximum-scale=1, user-scalable=no">
      <style>
        html, body, #c { margin: 0; padding: 0; width: 100%; height: 100%; background: transparent; }
      </style>
      <script src="echarts.min.js"></script>
      <script src="chart-theme.js"></script>
    </head>
    <body>
      <div id="c"></div>
      <script>
        (function () {
          var el = document.getElementById('c');
          // 'transparent' as the surface: this is a live canvas over the app's own
          // background, so fills need no opaque gap color to blend against.
          var theme = mbChartTheme($dark, {
            ink: '$ink', inkDim: '$inkDim', axis: '$axis', grid: '$grid',
            surface: 'transparent'
          });
          var chart = echarts.init(el, theme, { renderer: 'canvas' });
          try {
            chart.setOption(mbPrepare($optionJson));
          } catch (e) {
            el.innerHTML = '<pre style="color:$ink;white-space:pre-wrap">' + e + '</pre>';
          }
          window.addEventListener('resize', function () { chart.resize(); });
        })();
      </script>
    </body>
    </html>
""".trimIndent()

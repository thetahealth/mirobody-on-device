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
/** The chart's height, in dp — and, because the viewport is 1:1, in CSS px too. */
private const val CHART_HEIGHT_DP = 240

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
            .height(CHART_HEIGHT_DP.dp),
        factory = { context ->
            WebView(context).apply {
                setBackgroundColor(AndroidColor.TRANSPARENT)
                settings.javaScriptEnabled = true
                settings.allowFileAccess = true
                settings.allowFileAccessFromFileURLs = true
                // Without these the viewport meta tag is IGNORED and the page lays out
                // against a default ~980px-wide viewport, so one CSS pixel stops being
                // one dp and the chart is sized for a screen this phone doesn't have.
                settings.useWideViewPort = true
                settings.loadWithOverviewMode = true
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
        html, body { margin: 0; padding: 0; background: transparent; }
        /* An EXPLICIT height, not 100%. echarts measures the container at init and
           sizes a canvas to it once; a percentage height resolves to 0 while the
           WebView is still laying out, and a 0-tall canvas never grows back because
           no resize event follows. The host knows the height it gave us, so it says
           so — the viewport is 1:1 with dp (see useWideViewPort). */
        #c { width: 100%; height: ${CHART_HEIGHT_DP}px; }
        #e { margin: 0; padding: 8px; color: $ink; font: 12px sans-serif; white-space: pre-wrap; }
      </style>
      <script>
        // FIRST, before the two libraries: an error hook installed after them cannot
        // see them fail. Two different failures have to be told apart, and they use
        // different mechanisms — window.onerror never fires for a subresource that
        // could not be fetched, and a capture-phase listener is how you see that one.
        window.__mbError = null;
        window.onerror = function (message, source, line) {
          window.__mbError = 'threw: ' + message +
            ' (' + String(source || '?').split('/').pop() + ':' + line + ')';
          return true;
        };
        window.addEventListener('error', function (e) {
          var t = e.target;
          if (t && t.tagName === 'SCRIPT') {
            window.__mbError = 'could not fetch ' + String(t.src).split('/').pop();
          }
        }, true);

        // chart-theme.js ends in `})(globalThis)`, and `globalThis` is ES2020
        // (Chrome 71+). On a WebView older than that, evaluating it throws a
        // ReferenceError and the whole file dies before defining anything — while
        // echarts.min.js, transpiled to ES5, loads perfectly. That asymmetry is
        // exactly what the device reported ("chart-theme.js did not load").
        //
        // Shimmed HERE rather than in that file: it is vendored byte-identical to
        // four clients (harmony, htdoc, ios and this one), so the WebView's gap is
        // the host's problem to close. `typeof` on an undeclared name does not
        // throw, so this is safe to ask; on a current engine it is a no-op.
        if (typeof globalThis === 'undefined') { window.globalThis = window; }
      </script>
      <script src="echarts.min.js"></script>
      <script src="chart-theme.js"></script>
    </head>
    <body>
      <div id="c"></div>
      <script>
        // A blank chart used to be the failure mode for every cause at once: a script
        // that did not load, an option echarts rejected, a container with no height.
        // Anything that goes wrong now SAYS so, in the space the figure would occupy.
        function mbFail(what) {
          var el = document.getElementById('c');
          var why = window.__mbError ? (what + ' — ' + window.__mbError) : what;
          if (el) { el.innerHTML = '<pre id="e">chart: ' + why + '</pre>'; }
        }

        function mbDraw() {
          if (typeof echarts === 'undefined') { mbFail('echarts.min.js unavailable'); return; }
          if (typeof mbChartTheme === 'undefined') { mbFail('chart-theme.js unavailable'); return; }
          var el = document.getElementById('c');
          // 'transparent' as the surface: this is a live canvas over the app's own
          // background, so fills need no opaque gap color to blend against.
          var theme = mbChartTheme($dark, {
            ink: '$ink', inkDim: '$inkDim', axis: '$axis', grid: '$grid',
            surface: 'transparent'
          });
          var chart = echarts.init(el, theme, { renderer: 'canvas' });
          chart.setOption(mbPrepare($optionJson));
          // The container is explicitly sized, so this is belt-and-braces for a
          // device that reports a stale width on the first frame.
          chart.resize();
          window.addEventListener('resize', function () { chart.resize(); });
        }

        function mbRun() {
          // onerror only RECORDS now (it has to, to survive until mbDraw can report
          // it), so a throw in here would otherwise be silent again.
          try { mbDraw(); } catch (err) { mbFail(String(err)); }
        }

        // Draw after layout, never during parsing: at parse time the container's
        // computed height is not yet trustworthy, which is the other half of the
        // percentage-height problem above.
        if (document.readyState === 'complete') {
          mbRun();
        } else {
          window.addEventListener('load', mbRun);
        }
      </script>
    </body>
    </html>
""".trimIndent()

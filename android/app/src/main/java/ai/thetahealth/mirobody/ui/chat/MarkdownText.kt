package ai.thetahealth.mirobody.ui.chat

import android.text.util.Linkify
import android.util.TypedValue
import android.widget.TextView
import androidx.compose.material3.LocalContentColor
import androidx.compose.material3.MaterialTheme
import androidx.compose.runtime.Composable
import androidx.compose.runtime.remember
import androidx.compose.ui.Modifier
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.graphics.toArgb
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.platform.LocalDensity
import androidx.compose.ui.text.TextStyle
import androidx.compose.ui.viewinterop.AndroidView
import io.noties.markwon.Markwon
import io.noties.markwon.ext.latex.JLatexMathPlugin
import io.noties.markwon.ext.strikethrough.StrikethroughPlugin
import io.noties.markwon.ext.tables.TablePlugin
import io.noties.markwon.html.HtmlPlugin
import io.noties.markwon.inlineparser.MarkwonInlineParserPlugin
import io.noties.markwon.linkify.LinkifyPlugin

/**
 * Renders chat content as Markdown with LaTeX math by hosting a Markwon-driven
 * [TextView] via [AndroidView]. Streaming token deltas re-parse the full buffer on
 * every update — acceptable for typical LLM response sizes.
 *
 * Recognized math: `$$...$$` (block) and `$...$` (inline, enabled below). Other
 * features: GFM tables, strikethrough, inline HTML, auto-linked URLs.
 */
@Composable
fun MarkdownText(
    text: String,
    modifier: Modifier = Modifier,
    color: Color = LocalContentColor.current,
    style: TextStyle = MaterialTheme.typography.bodyMedium,
) {
    val context = LocalContext.current
    val density = LocalDensity.current
    val textSizePx = with(density) { style.fontSize.toPx() }
    val markwon = remember(context, textSizePx) {
        Markwon.builder(context)
            // Required by JLatexMathPlugin.inlinesEnabled(true) — it registers a custom
            // inline parser for `$...$` math, which only works if the inline-parser
            // plugin is present in the builder.
            .usePlugin(MarkwonInlineParserPlugin.create())
            .usePlugin(StrikethroughPlugin.create())
            .usePlugin(TablePlugin.create(context))
            .usePlugin(HtmlPlugin.create())
            // LinkifyPlugin.create() defaults to Linkify.ALL, which also matches
            // MAP_ADDRESSES (treats date-like strings as addresses → underlined) and
            // PHONE_NUMBERS, both of which launch implicit intents (geo: / tel:) that
            // can ActivityNotFoundException-crash on devices without a handler. Keep
            // it to URLs and emails — Markdown already handles explicit links.
            .usePlugin(LinkifyPlugin.create(Linkify.WEB_URLS or Linkify.EMAIL_ADDRESSES))
            .usePlugin(JLatexMathPlugin.create(textSizePx) { builder ->
                builder.inlinesEnabled(true)
            })
            .build()
    }
    val argb = color.toArgb()
    // Size the TextView in *px* off the Compose LocalDensity (textSizePx already
    // folds in the in-app font-size offset via density.fontScale). TextView.textSize
    // takes sp resolved against the *system* font scale, which the in-app slider
    // never touches -- so setting sp here would leave the bubbles fixed while the
    // rest of the UI rescales.
    AndroidView(
        modifier = modifier,
        factory = { ctx ->
            TextView(ctx).apply {
                setTextColor(argb)
                setTextSize(TypedValue.COMPLEX_UNIT_PX, textSizePx)
            }
        },
        update = { tv ->
            tv.setTextColor(argb)
            tv.setTextSize(TypedValue.COMPLEX_UNIT_PX, textSizePx)
            markwon.setMarkdown(tv, text)
        },
    )
}

package ai.thetahealth.mirobody.ui.chat

import android.content.ActivityNotFoundException
import android.content.Intent
import android.net.Uri
import android.provider.Browser
import android.text.util.Linkify
import android.util.Log
import android.util.TypedValue
import android.view.View
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
import io.noties.markwon.AbstractMarkwonPlugin
import io.noties.markwon.LinkResolver
import io.noties.markwon.Markwon
import io.noties.markwon.MarkwonConfiguration
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
 * Recognized math: `$$…$$` (block and inline) and `$…$` (inline). The single-dollar form
 * is ours — see [DollarMathInlineProcessor] for why Markwon's own inline processor cannot
 * provide it and why it is displaced rather than added to. Other features: GFM tables,
 * strikethrough, inline HTML, auto-linked URLs.
 *
 * This composable renders **one markdown segment**. A message that also carries an SVG or
 * chart fence is split by [splitMessage] first — see `MessageBody` in ChatScreen.
 */
@Composable
fun MarkdownText(
    text: String,
    modifier: Modifier = Modifier,
    color: Color = LocalContentColor.current,
    style: TextStyle = MaterialTheme.typography.bodyMedium,
    // A long press on the rendered text. It has to be handled by the TextView rather
    // than by a Compose modifier on some ancestor: this is an AndroidView, it consumes
    // the touch stream it handles (Markwon installs a MovementMethod for links), and a
    // `combinedClickable` further out would never see the gesture.
    onLongClick: (() -> Unit)? = null,
) {
    val context = LocalContext.current
    val density = LocalDensity.current
    val textSizePx = with(density) { style.fontSize.toPx() }
    val markwon = remember(context, textSizePx) {
        Markwon.builder(context)
            // Our `$` processor is added to the factory builder at CREATION time, which
            // puts it ahead of the one JLatexMathPlugin registers for the same character.
            // The order is load-bearing: processors are tried in registration order and
            // the first non-null node wins, and ours always returns one. See
            // DollarMathInlineProcessor for the text-swallowing bug that ordering avoids.
            .usePlugin(
                MarkwonInlineParserPlugin.create { factoryBuilder ->
                    factoryBuilder.addInlineProcessor(DollarMathInlineProcessor())
                }
            )
            .usePlugin(StrikethroughPlugin.create())
            .usePlugin(TablePlugin.create(context))
            .usePlugin(HtmlPlugin.create())
            // LinkifyPlugin.create() defaults to Linkify.ALL, which also matches
            // MAP_ADDRESSES (treats date-like strings as addresses → underlined) and
            // PHONE_NUMBERS, both of which launch implicit intents (geo: / tel:) that
            // can ActivityNotFoundException-crash on devices without a handler. Keep
            // it to URLs and emails — Markdown already handles explicit links.
            .usePlugin(LinkifyPlugin.create(Linkify.WEB_URLS or Linkify.EMAIL_ADDRESSES))
            .usePlugin(AllowlistLinkPlugin())
            // inlinesEnabled(true) stays on: it is also what registers the visitor and
            // span for an inline math node, which our own processor produces.
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
            if (onLongClick == null) {
                tv.setOnLongClickListener(null)
                tv.isLongClickable = false
            } else {
                // Returning true marks the gesture consumed. The View framework has
                // already fired HapticFeedbackConstants.LONG_PRESS by the time this
                // runs — performLongClick() buzzes before it calls the listener — so
                // the callback must not buzz again.
                tv.setOnLongClickListener { onLongClick(); true }
            }
        },
    )
}

/** Installs [AllowlistLinkResolver] in place of Markwon's default. */
private class AllowlistLinkPlugin : AbstractMarkwonPlugin() {
    override fun configureConfiguration(builder: MarkwonConfiguration.Builder) {
        builder.linkResolver(AllowlistLinkResolver())
    }
}

/**
 * Opens a tapped link only if its scheme is one we are willing to hand to the system.
 *
 * Markwon's `LinkResolverDef` fires `ACTION_VIEW` on whatever the link says — and coerces
 * a scheme-less link to `https` on the way. In a chat client the link text is written by
 * a language model, so that is an implicit intent chosen by generated output: `file://`,
 * a custom scheme, or an `intent://` URI would all be launched. Harmony allowlists the
 * same three schemes for the same reason (see docs/markdown.md §5).
 *
 * A refused link does nothing rather than showing an error: the reader did not author it
 * and has nothing to fix.
 */
private class AllowlistLinkResolver : LinkResolver {

    override fun resolve(view: View, link: String) {
        val uri = Uri.parse(link)
        // No scheme is the `www.foo.com` case — reading it as https is what the user
        // means, and it lands inside the allowlist rather than bypassing it.
        val target = if (uri.scheme.isNullOrEmpty()) uri.buildUpon().scheme("https").build() else uri
        if (target.scheme?.lowercase() !in ALLOWED) {
            Log.w(TAG, "refusing link scheme: ${target.scheme}")
            return
        }
        val context = view.context
        val intent = Intent(Intent.ACTION_VIEW, target)
            .putExtra(Browser.EXTRA_APPLICATION_ID, context.packageName)
        try {
            context.startActivity(intent)
        } catch (e: ActivityNotFoundException) {
            Log.w(TAG, "no activity for link: $link")
        }
    }

    private companion object {
        const val TAG = "MarkdownLinks"
        val ALLOWED = setOf("http", "https", "mailto")
    }
}

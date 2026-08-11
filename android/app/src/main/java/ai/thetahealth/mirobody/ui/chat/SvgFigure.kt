package ai.thetahealth.mirobody.ui.chat

import androidx.compose.foundation.layout.aspectRatio
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.runtime.Composable
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.setValue
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.clip
import androidx.compose.ui.layout.ContentScale
import androidx.compose.ui.platform.LocalContext
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.ui.unit.dp
import coil.compose.AsyncImage
import coil.request.ImageRequest

/**
 * A ` ```svg ` fenced block drawn as an actual figure.
 *
 * No new renderer is needed: [ai.thetahealth.mirobody.MirobodyApp] already registers
 * Coil's `SvgDecoder` app-wide, and handing it the source as a `ByteArray` is the same
 * trick [chatImageModel] uses for a raw-SVG chunk — the decoder sniffs `<svg` in the
 * first 1024 bytes. What this file adds is the part that is *not* free: deciding whether
 * we are willing to draw model-authored markup at all, and what shape to give it.
 *
 * The three rules are HarmonyOS's (`core/RenderHost.ets`), ported so both hand-written
 * clients refuse the same sources.
 */

/**
 * Is this SVG source one we are willing to hand to the rasterizer?
 *
 * VALIDATE AND REFUSE, never rewrite. The source is model-authored and untrusted, and
 * stripping dangerous constructs out of untrusted markup by text surgery is a game of
 * whack-a-mole against an adversary who writes the input. Refusing is total, auditable,
 * and costs a model nothing: a diagram needs none of this.
 *
 * AndroidSVG (behind Coil's decoder) rasterizes directly rather than through a browser,
 * so there is no script engine to attack. What is left is what this checks for:
 *
 *   external references  `<image href="https://…">` would fetch when the reply is read,
 *                        which is the read-receipt problem markdown images are refused
 *                        for. Fragment refs (`#gradient`) are fine and common, so they
 *                        stay allowed.
 *   DOCTYPE / ENTITY     entity expansion is a parser-level denial of service that no
 *                        renderer feature depends on.
 *   script, foreignObject, use
 *                        inert here by construction, refused anyway — the cost is three
 *                        string searches and it stops this from silently becoming unsafe
 *                        if the source ever reaches a WebView.
 */
internal fun svgIsSafe(source: String): Boolean {
    val s = source.lowercase()
    if (s.contains("<script") || s.contains("<foreignobject") ||
        s.contains("<!doctype") || s.contains("<!entity") || s.contains("<use")
    ) {
        return false
    }
    // Any on*= handler attribute. Checked as ` on…=` so "button-on=" and prose
    // containing "on" do not trip it.
    for (i in 1 until (s.length - 3).coerceAtLeast(1)) {
        val before = s[i - 1]
        if ((before == ' ' || before == '\t' || before == '\n') && s[i] == 'o' && s[i + 1] == 'n') {
            var j = i + 2
            while (j < s.length && s[j] != '=' && s[j] != ' ' && s[j] != '>') j++
            if (j < s.length && s[j] == '=') return false
        }
    }
    return !hasExternalRef(s, "href") && !hasExternalRef(s, "src")
}

private fun hasExternalRef(lower: String, attr: String): Boolean {
    var from = 0
    while (true) {
        val at = lower.indexOf("$attr=", from)
        if (at < 0) return false
        var j = at + attr.length + 1
        while (j < lower.length && (lower[j] == '"' || lower[j] == '\'')) j++
        if (j < lower.length && lower[j] != '#') return true
        from = at + attr.length + 1
    }
}

/**
 * The quoted value of an attribute, or "".
 *
 * The name must start a real attribute — preceded by whitespace or `<` — because a bare
 * indexOf("width=") also matches `stroke-width="2"`, and a figure sized off a stroke
 * would be shaped by whichever rectangle happened to be drawn first.
 */
private fun attrValue(lower: String, source: String, attr: String): String {
    var from = 0
    while (true) {
        val at = lower.indexOf("$attr=", from)
        if (at < 0) return ""
        val before = if (at == 0) '<' else lower[at - 1]
        if (before == ' ' || before == '\t' || before == '\n' || before == '<') {
            val q = source.getOrNull(at + attr.length + 1)
            if (q == '"' || q == '\'') {
                val end = source.indexOf(q!!, at + attr.length + 2)
                if (end > 0) return source.substring(at + attr.length + 2, end)
            }
            return ""
        }
        from = at + attr.length + 1
    }
}

/** An absolute CSS length -> px. A percentage (or anything unusable) -> 0. */
private fun plainPixels(v: String): Float {
    val t = v.trim()
    if (t.isEmpty() || t.endsWith("%")) return 0f
    val n = t.takeWhile { it.isDigit() || it == '.' || it == '-' }.toFloatOrNull() ?: return 0f
    return if (n <= 0f) 0f else n
}

/**
 * The figure's aspect ratio (width / height).
 *
 * SIZED BY THE BUBBLE, SHAPED BY THE SOURCE. Width is 100% of the bubble and only the
 * ratio comes from the source, because a model picks a canvas without knowing the screen,
 * so its declared pixels are not honoured.
 *
 * viewBox first, and width/height only as a fallback: a hand-written SVG reliably carries
 * a viewBox, while its width/height are as often absent or a percentage — and a
 * percentage says nothing about shape, so it counts as unknown rather than as the number
 * in front of the sign. No size at all -> 4:3.
 */
internal fun svgAspectRatio(source: String): Float {
    val lower = source.lowercase()

    val vb = attrValue(lower, source, "viewbox")
    if (vb.isNotEmpty()) {
        val parts = vb.split(' ', ',', '\t', '\n').filter { it.isNotEmpty() }
        if (parts.size == 4) {
            val w = parts[2].toFloatOrNull()
            val h = parts[3].toFloatOrNull()
            if (w != null && h != null && w > 0f && h > 0f) return w / h
        }
    }

    val w = plainPixels(attrValue(lower, source, "width"))
    val h = plainPixels(attrValue(lower, source, "height"))
    return if (w > 0f && h > 0f) w / h else 4f / 3f
}

/**
 * Draw an SVG fence body, or fall back to its source.
 *
 * **onError demotes.** Validation proves only that we are *willing* to draw the source,
 * not that AndroidSVG can parse it. A figure that fails to decode marks itself failed and
 * renders as a code block instead, so the worst case is exactly what the reader had
 * before this feature existed.
 */
@Composable
fun SvgFigure(source: String, modifier: Modifier = Modifier) {
    var failed by remember(source) { mutableStateOf(!svgIsSafe(source)) }
    if (failed) {
        // Fenced with a run longer than any in the source, so a refused figure cannot
        // close its own code block early — the fallback is exactly the case where the
        // source is malformed, so it gets no benefit of the doubt.
        val fence = remember(source) { "`".repeat(maxOf(3, longestBacktickRun(source) + 1)) }
        MarkdownText(text = "$fence\n$source\n$fence", modifier = modifier)
        return
    }
    val context = LocalContext.current
    val bytes = remember(source) { source.toByteArray() }
    val ratio = remember(source) { svgAspectRatio(source) }
    AsyncImage(
        model = ImageRequest.Builder(context).data(bytes).build(),
        contentDescription = null,
        contentScale = ContentScale.FillWidth,
        onError = { failed = true },
        modifier = modifier
            .fillMaxWidth()
            .aspectRatio(ratio)
            .clip(RoundedCornerShape(8.dp)),
    )
}

private fun longestBacktickRun(s: String): Int {
    var best = 0
    var run = 0
    for (c in s) {
        if (c == '`') { run++; if (run > best) best = run } else run = 0
    }
    return best
}

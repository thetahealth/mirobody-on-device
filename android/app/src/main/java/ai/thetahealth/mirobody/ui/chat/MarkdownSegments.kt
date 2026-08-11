package ai.thetahealth.mirobody.ui.chat

/**
 * A reply body split into the pieces that need different renderers.
 *
 * Markwon draws a TextView, an SVG figure is a Coil image and a chart is a WebView, so a
 * message carrying either has to be broken up before it can be laid out. Everything that
 * is not one of our two fences stays [Markdown] — including fences we do not draw, which
 * remain ordinary code blocks.
 */
sealed interface MessageSegment {
    data class Markdown(val text: String) : MessageSegment
    data class Svg(val source: String) : MessageSegment
    data class Chart(val optionJson: String) : MessageSegment
}

/** One fenced block's opening line: which character, how many, and the info string. */
private data class Fence(val char: Char, val count: Int, val info: String)

/**
 * Split a reply into renderable segments.
 *
 * STREAMING IS WHAT SHAPES THIS. A message is re-split on every token, so an **unclosed**
 * fence is deliberately left as markdown: half an SVG is not a figure and half a JSON
 * option is not a chart, and the reader is better served watching source arrive than
 * watching a figure flicker in and out. A block becomes a figure the moment its closing
 * fence lands, and not before.
 *
 * Fences that are not ours are still parsed — and skipped over — so a ` ```svg ` written
 * *inside* a ` ```markdown ` example stays example text rather than being lifted out and
 * drawn.
 */
fun splitMessage(text: String): List<MessageSegment> {
    if (!text.contains("```") && !text.contains("~~~")) {
        return listOf(MessageSegment.Markdown(text))
    }

    val out = ArrayList<MessageSegment>()
    val prose = StringBuilder()
    val lines = text.split("\n")
    var i = 0

    while (i < lines.size) {
        val open = fenceOpen(lines[i])
        if (open == null) {
            prose.append(lines[i]).append('\n')
            i++
            continue
        }

        var closed = -1
        var j = i + 1
        while (j < lines.size) {
            if (fenceCloses(lines[j], open)) { closed = j; break }
            j++
        }
        if (closed < 0) {
            // Still streaming. Keep the rest as text; the figure appears when it closes.
            while (i < lines.size) { prose.append(lines[i]).append('\n'); i++ }
            break
        }

        val body = lines.subList(i + 1, closed).joinToString("\n")
        when (open.info) {
            "svg" -> { flush(prose, out); out += MessageSegment.Svg(body) }
            "echarts" -> { flush(prose, out); out += MessageSegment.Chart(body) }
            else -> for (k in i..closed) prose.append(lines[k]).append('\n')
        }
        i = closed + 1
    }

    flush(prose, out)
    return if (out.isEmpty()) listOf(MessageSegment.Markdown(text)) else out
}

private fun flush(prose: StringBuilder, out: MutableList<MessageSegment>) {
    if (prose.isNotBlank()) out += MessageSegment.Markdown(prose.toString().trimEnd('\n'))
    prose.setLength(0)
}

/**
 * The fence this line opens, or null.
 *
 * Up to three leading spaces, then three or more of the same fence character — a fourth
 * space makes it an indented code block instead, and both the character and the count
 * matter, which is what lets a ` ```` ` block contain a ` ``` ` line.
 */
private fun fenceOpen(line: String): Fence? {
    var i = 0
    while (i < 3 && i < line.length && line[i] == ' ') i++
    if (i >= line.length) return null
    val c = line[i]
    if (c != '`' && c != '~') return null
    var n = 0
    while (i + n < line.length && line[i + n] == c) n++
    if (n < 3) return null
    val info = line.substring(i + n).trim()
    // CommonMark: a backtick fence's info string may not itself contain a backtick.
    if (c == '`' && info.contains('`')) return null
    return Fence(c, n, info.lowercase())
}

/** Does this line close [open]? Same character, at least as many, nothing else on it. */
private fun fenceCloses(line: String, open: Fence): Boolean {
    var i = 0
    while (i < 3 && i < line.length && line[i] == ' ') i++
    var n = 0
    while (i + n < line.length && line[i + n] == open.char) n++
    if (n < open.count) return false
    return line.substring(i + n).isBlank()
}

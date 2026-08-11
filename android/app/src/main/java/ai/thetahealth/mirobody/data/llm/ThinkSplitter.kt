package ai.thetahealth.mirobody.data.llm

/**
 * Splits a model's `<think>…</think>` out of its answer, across a streamed feed.
 *
 * Qwen3 is a hybrid reasoning model: it writes its reasoning inline, wrapped in these
 * markers, and everything after `</think>` is the actual reply. Without this the tags and
 * the whole train of thought land in the message body as literal text.
 *
 * A port of `ThinkSplitter` in `src/llm/local.cpp`, which is how HarmonyOS gets this for
 * free — its on-device lane runs through the C++ core, where the split already happens.
 * Android's runs through LiteRT-LM in Kotlin and reaches no such code, so the logic has
 * to exist twice; keep the two in step.
 *
 * THE HARD PART IS THE BOUNDARY. A marker can arrive split across two chunks (`<thi` then
 * `nk>`), so a naive `contains` check emits half a tag as prose and then never matches.
 * [emittable] is the answer: hold back the longest suffix of the buffer that could still
 * become the tag, and release it once it cannot.
 *
 * The C++ original also guards partial UTF-8 sequences; here it is unnecessary — LiteRT-LM
 * hands over decoded `String`s, so no multi-byte character can arrive half-formed.
 */
class ThinkSplitter {

    /** One releasable run of text, and which stream it belongs to. */
    data class Piece(val thinking: Boolean, val text: String)

    private val buf = StringBuilder()
    private var inThink = false

    /**
     * Feed one delta; returns the pieces that are now safe to release, in order. The
     * markers themselves are never returned.
     *
     * Returning rather than invoking a callback keeps this a pure function of the feed
     * sequence: the caller emits into a Flow (a suspend context) and the splitter needs
     * to know nothing about coroutines to be tested exhaustively.
     */
    fun feed(chunk: String): List<Piece> {
        val out = ArrayList<Piece>()
        buf.append(chunk)
        while (true) {
            val tag = if (inThink) THINK_CLOSE else THINK_OPEN
            val at = buf.indexOf(tag)
            if (at >= 0) {
                // Text before the marker belongs to whichever stream we were in.
                if (at > 0) out += Piece(inThink, buf.substring(0, at))
                buf.delete(0, at + tag.length)
                inThink = !inThink
                continue                       // a reply can hold several blocks
            }
            val n = emittable(buf, tag)
            if (n > 0) {
                out += Piece(inThink, buf.substring(0, n))
                buf.delete(0, n)
            }
            return out
        }
    }

    /**
     * Release whatever is held back, at end of stream.
     *
     * Anything still buffered was a possible tag prefix that never completed — a reply
     * genuinely ending in "<" — so it is text after all, and dropping it would silently
     * eat characters.
     */
    fun flush(): List<Piece> {
        if (buf.isEmpty()) return emptyList()
        val out = buf.toString()
        buf.setLength(0)
        return listOf(Piece(inThink, out))
    }

    private companion object {
        const val THINK_OPEN = "<think>"
        const val THINK_CLOSE = "</think>"

        /**
         * How much of [s] may be emitted without risking splitting [tag] across two
         * feeds: everything except the longest suffix of [s] that is also a prefix of
         * [tag].
         */
        fun emittable(s: CharSequence, tag: String): Int {
            val max = minOf(s.length, tag.length - 1)
            for (k in max downTo 1) {
                if (regionMatches(s, s.length - k, tag, k)) return s.length - k
            }
            return s.length
        }

        /** Does the last [len] chars of [s] starting at [from] equal `tag[0 until len]`? */
        fun regionMatches(s: CharSequence, from: Int, tag: String, len: Int): Boolean {
            for (i in 0 until len) if (s[from + i] != tag[i]) return false
            return true
        }
    }
}

package ai.thetahealth.mirobody.data.llm

import org.junit.Assert.assertEquals
import org.junit.Test

/**
 * The splitter's whole job is surviving arbitrary chunk boundaries, which is exactly what
 * a hand-written example never exercises — so the boundary cases are driven by feeding the
 * SAME input at every possible split.
 */
class ThinkSplitterTest {

    /** Feed [text] one chunk at a time and collect what came out on each stream. */
    private fun run(vararg chunks: String): Pair<String, String> {
        val think = StringBuilder()
        val reply = StringBuilder()
        val s = ThinkSplitter()
        val take = { pieces: List<ThinkSplitter.Piece> ->
            for (p in pieces) (if (p.thinking) think else reply).append(p.text)
        }
        for (c in chunks) take(s.feed(c))
        take(s.flush())
        return think.toString() to reply.toString()
    }

    @Test
    fun `reasoning goes to thinking, the rest to the reply`() {
        val (think, reply) = run("<think>weighing it up</think>The answer is 42.")
        assertEquals("weighing it up", think)
        assertEquals("The answer is 42.", reply)
    }

    @Test
    fun `markers themselves are never emitted`() {
        val (think, reply) = run("<think>a</think>b")
        assertEquals("a", think)
        assertEquals("b", reply)
    }

    @Test
    fun `text with no markers is all reply`() {
        val (think, reply) = run("just an answer")
        assertEquals("", think)
        assertEquals("just an answer", reply)
    }

    @Test
    fun `a reply can hold several blocks`() {
        val (think, reply) = run("<think>one</think>A<think>two</think>B")
        assertEquals("onetwo", think)
        assertEquals("AB", reply)
    }

    @Test
    fun `an empty think block yields nothing to show`() {
        // A model that declines to reason still emits the pair.
        val (think, reply) = run("<think></think>answer")
        assertEquals("", think)
        assertEquals("answer", reply)
    }

    @Test
    fun `every chunk boundary produces the same result`() {
        val whole = "<think>reasoning here</think>The reply.<think>more</think> Done."
        val (expectThink, expectReply) = run(whole)
        for (cut in 1 until whole.length) {
            val (think, reply) = run(whole.substring(0, cut), whole.substring(cut))
            assertEquals("split at $cut: thinking", expectThink, think)
            assertEquals("split at $cut: reply", expectReply, reply)
        }
    }

    @Test
    fun `one character at a time is still correct`() {
        // The worst case a token stream can produce, and the one that breaks any
        // implementation that looks for the tag without holding back a prefix.
        val whole = "<think>abc</think>xyz"
        val (think, reply) = run(*whole.map { it.toString() }.toTypedArray())
        assertEquals("abc", think)
        assertEquals("xyz", reply)
    }

    @Test
    fun `an unterminated tag prefix is text, not a swallowed tail`() {
        // A reply that genuinely ends in "<" or "<thi" must not lose those characters:
        // they were held back as a possible marker and the stream ended instead.
        assertEquals("" to "answer <", run("answer <"))
        assertEquals("" to "answer <thi", run("answer <thi"))
        assertEquals("" to "5 < 7", run("5 < 7"))
    }

    @Test
    fun `an unclosed think block keeps its text on the thinking stream`() {
        // Generation cut short mid-reasoning: what arrived is still reasoning, not reply.
        val (think, reply) = run("<think>half a thought")
        assertEquals("half a thought", think)
        assertEquals("", reply)
    }
}

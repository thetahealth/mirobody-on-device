package ai.thetahealth.mirobody.ui.chat

import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertTrue
import org.junit.Test

/**
 * The parts of the reply renderer that are decisions rather than dependencies: which
 * `$…$` pairs are math, which fenced blocks become figures, and which SVG sources we are
 * willing to draw. All pure logic, so this runs without a device, a model or a network —
 * the same reason the `/help` document doubles as an on-device check of the rest.
 */
class MarkdownRenderingTest {

    // -- $…$ vs currency ----------------------------------------------------------

    @Test
    fun `tex metacharacter is decisive`() {
        assertTrue(looksLikeTex("BMI = w / h^2"))
        assertTrue(looksLikeTex("Ca^{2+}"))
        assertTrue(looksLikeTex("\\alpha"))
        assertTrue(looksLikeTex("S_{cr}"))
    }

    @Test
    fun `bare variables are math`() {
        assertTrue(looksLikeTex("w"))
        assertTrue(looksLikeTex("h"))
        assertTrue(looksLikeTex("x + y"))
    }

    @Test
    fun `prices are not math`() {
        // "costs $100 and $200" leaves the body "100 and ".
        assertFalse(looksLikeTex("100 and "))
        assertFalse(looksLikeTex("1,200.50"))
    }

    @Test
    fun `prose between two dollars is not math`() {
        assertFalse(looksLikeTex("five or "))
        assertFalse(looksLikeTex("的检查和一项 "))
        assertFalse(looksLikeTex(""))
        // Long enough that it is a sentence, whatever it looks like.
        assertFalse(looksLikeTex("a".repeat(49)))
    }

    @Test
    fun `a dollar pair does not span a line break`() {
        assertFalse(looksLikeTex("abc\ndef"))
    }

    // -- segment splitting --------------------------------------------------------

    @Test
    fun `plain text is one markdown segment`() {
        val segments = splitMessage("hello **world**")
        assertEquals(1, segments.size)
        assertTrue(segments[0] is MessageSegment.Markdown)
    }

    @Test
    fun `a figure lands between the paragraphs around it`() {
        val segments = splitMessage(
            """
            before

            ```svg
            <svg viewBox="0 0 10 5"></svg>
            ```

            after
            """.trimIndent()
        )
        assertEquals(3, segments.size)
        assertTrue(segments[0] is MessageSegment.Markdown)
        assertEquals("<svg viewBox=\"0 0 10 5\"></svg>", (segments[1] as MessageSegment.Svg).source)
        assertTrue((segments[2] as MessageSegment.Markdown).text.contains("after"))
    }

    @Test
    fun `an echarts fence becomes a chart`() {
        val segments = splitMessage("```echarts\n{\"series\":[]}\n```")
        assertEquals(1, segments.size)
        assertEquals("{\"series\":[]}", (segments[0] as MessageSegment.Chart).optionJson)
    }

    @Test
    fun `an unclosed fence stays text while it streams`() {
        val segments = splitMessage("intro\n\n```svg\n<svg viewBox=\"0 0 10 5\">")
        assertEquals(1, segments.size)
        assertTrue(segments[0] is MessageSegment.Markdown)
    }

    @Test
    fun `an ordinary code fence is left alone`() {
        val segments = splitMessage("```python\nprint(1)\n```")
        assertEquals(1, segments.size)
        assertTrue(segments[0] is MessageSegment.Markdown)
    }

    @Test
    fun `a nested fence is not lifted out of its example`() {
        val text = "````markdown\n```svg\n<svg/>\n```\n````"
        val segments = splitMessage(text)
        assertEquals(1, segments.size)
        assertTrue((segments[0] as MessageSegment.Markdown).text.contains("<svg/>"))
    }

    // -- SVG: validate and refuse -------------------------------------------------

    @Test
    fun `a plain diagram is drawable`() {
        assertTrue(svgIsSafe("<svg viewBox=\"0 0 10 5\"><rect fill=\"#123456\"/></svg>"))
        // Same-document fragment references are ordinary and stay allowed.
        assertTrue(svgIsSafe("<svg><rect fill=\"url(#g)\" clip-path=\"#c\" href=\"#a\"/></svg>"))
    }

    @Test
    fun `dangerous constructs are refused`() {
        assertFalse(svgIsSafe("<svg><script>alert(1)</script></svg>"))
        assertFalse(svgIsSafe("<svg><foreignObject><b>x</b></foreignObject></svg>"))
        assertFalse(svgIsSafe("<svg><use href=\"#a\"/></svg>"))
        assertFalse(svgIsSafe("<!DOCTYPE svg><svg/>"))
        assertFalse(svgIsSafe("<svg onload=\"x()\"/>"))
        assertFalse(svgIsSafe("<svg><rect onclick=\"x()\"/></svg>"))
    }

    @Test
    fun `external references are refused`() {
        assertFalse(svgIsSafe("<svg><image href=\"https://example.com/p.png\"/></svg>"))
        assertFalse(svgIsSafe("<svg><image src=\"http://example.com/p.png\"/></svg>"))
    }

    // -- SVG: shaped by the source ------------------------------------------------

    @Test
    fun `viewBox sets the aspect ratio`() {
        assertEquals(2f, svgAspectRatio("<svg viewBox=\"0 0 420 210\"/>"), 0.001f)
        assertEquals(2f, svgAspectRatio("<svg viewBox=\"0,0,420,210\"/>"), 0.001f)
    }

    @Test
    fun `width and height are the fallback, and a stroke is not a size`() {
        assertEquals(2f, svgAspectRatio("<svg width=\"200\" height=\"100\"/>"), 0.001f)
        // stroke-width must not be read as width — that would shape the figure by
        // whichever rectangle happened to be drawn first.
        assertEquals(4f / 3f, svgAspectRatio("<svg><rect stroke-width=\"2\"/></svg>"), 0.001f)
    }

    @Test
    fun `a percentage says nothing about shape`() {
        assertEquals(4f / 3f, svgAspectRatio("<svg width=\"100%\" height=\"100%\"/>"), 0.001f)
        assertEquals(4f / 3f, svgAspectRatio("<svg/>"), 0.001f)
    }

    // -- slash commands -----------------------------------------------------------

    @Test
    fun `the palette opens on a slash and narrows by prefix`() {
        assertEquals(3, SlashCommand.suggest("/").size)
        assertEquals(listOf(SLASH_NEW), SlashCommand.suggest("/n").map { it.name })
        assertTrue(SlashCommand.suggest("hello").isEmpty())
        // Past a space the user is writing a sentence, not choosing a command.
        assertTrue(SlashCommand.suggest("/help me").isEmpty())
    }

    @Test
    fun `probe runs but is never offered`() {
        // The whole distinction between undocumented and unavailable: typing it in full
        // works, the palette never suggests it.
        assertEquals(SLASH_PROBE, SlashCommand.match("/probe"))
        assertTrue(SlashCommand.suggest("/").none { it.name == SLASH_PROBE })
        assertTrue(SlashCommand.suggest("/p").isEmpty())
    }

    @Test
    fun `only a whole message is a command`() {
        assertEquals(SLASH_HELP, SlashCommand.match("  /HELP  "))
        assertEquals("", SlashCommand.match("what does /help do"))
        assertEquals("", SlashCommand.match("/nope"))
    }
}

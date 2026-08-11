package ai.thetahealth.mirobody.ui.chat

import java.io.File
import org.junit.Assert.assertEquals
import org.junit.Assert.assertTrue
import org.junit.Test

/**
 * The shipped `/help` documents are the on-device rendering check, so they are only
 * useful if they actually reach the renderers they are meant to exercise. This asserts
 * the split against the real asset rather than a hand-written sample: a fence that stops
 * being lifted out — or a document edited into no longer containing one — fails here
 * instead of quietly rendering as a JSON blob on a phone.
 */
class HelpDocumentTest {

    private fun helpDoc(name: String): String {
        val f = File("src/main/assets/help/$name")
        assertTrue("missing asset: ${f.absolutePath}", f.exists())
        return f.readText()
    }

    @Test
    fun `both languages ship`() {
        assertTrue(helpDoc("help-en.md").isNotEmpty())
        assertTrue(helpDoc("help-zh.md").isNotEmpty())
    }

    @Test
    fun `each guide yields a chart and a figure`() {
        for (name in listOf("help-en.md", "help-zh.md")) {
            val segments = splitMessage(helpDoc(name))
            val charts = segments.filterIsInstance<MessageSegment.Chart>()
            val figures = segments.filterIsInstance<MessageSegment.Svg>()
            assertEquals("$name: charts", 1, charts.size)
            assertEquals("$name: figures", 1, figures.size)
            assertTrue("$name: chart option is an object", charts[0].optionJson.trimStart().startsWith("{"))
            assertTrue("$name: figure is an svg", figures[0].source.trimStart().startsWith("<svg"))
            assertTrue("$name: figure is drawable", svgIsSafe(figures[0].source))
        }
    }

    @Test
    fun `the code fence stays a code block`() {
        val segments = splitMessage(helpDoc("help-en.md"))
        assertTrue(
            segments.filterIsInstance<MessageSegment.Markdown>()
                .any { it.text.contains("def bmi(") }
        )
    }

    @Test
    fun `the guide never mentions the probe`() {
        // /probe earns its usefulness by NOT being listed; documenting it makes it a
        // feature, and a page of device facts is not one. (HarmonyOS's check-markdown.js
        // asserts the same thing about its own help.)
        for (name in listOf("help-en.md", "help-zh.md")) {
            assertTrue("$name mentions /probe", !helpDoc(name).contains("/probe"))
        }
    }

    @Test
    fun `the figure sits between prose, not at the end`() {
        val segments = splitMessage(helpDoc("help-en.md"))
        val last = segments.last()
        assertTrue("the guide should end in prose", last is MessageSegment.Markdown)
    }
}

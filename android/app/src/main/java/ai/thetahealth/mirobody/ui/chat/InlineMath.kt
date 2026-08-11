package ai.thetahealth.mirobody.ui.chat

import io.noties.markwon.ext.latex.JLatexMathNode
import io.noties.markwon.inlineparser.InlineProcessor
import org.commonmark.node.Node

/**
 * Inline `$…$` and `$$…$$` math — a **total** handler for the `$` character.
 *
 * Markwon's own `JLatexMathInlineProcessor` is registered by
 * `JLatexMathPlugin.inlinesEnabled(true)` and matches `Pattern("(\\${2})([\\s\\S]+?)\\1")`
 * — two dollar signs, mandatory — so single-dollar math reached the screen with its
 * dollars intact. That is the visible half of the problem. The other half is worse:
 * `InlineProcessor.match()` runs `matcher.find()` over a *region* rather than anchoring
 * at the current index, so on `costs $5, and $$x$$` their processor is invoked at the
 * `$` before `5`, searches forward, matches the `$$x$$` further along, and advances the
 * index past it — silently swallowing ", and " out of the reply.
 *
 * Both go away by handling `$` completely here and never returning null: a non-null node
 * stops the processor loop, so theirs is never reached. This one is registered on the
 * factory builder at *creation* time (see MarkdownText), which puts it ahead of theirs in
 * the per-character list — the ordering the fix depends on.
 *
 * A `$` that opens nothing is returned as literal text, which is also what makes a
 * half-streamed `$x` render as written instead of eating the rest of the message.
 */
class DollarMathInlineProcessor : InlineProcessor() {

    override fun specialCharacter(): Char = '$'

    override fun parse(): Node? {
        val src = input
        val start = index

        // `$$…$$` — unambiguous, no currency reading to guard against.
        if (start + 1 < src.length && src[start + 1] == '$') {
            val close = src.indexOf("$$", start + 2)
            if (close > start + 2) {
                index = close + 2
                return mathNode(src.substring(start + 2, close))
            }
            // Unterminated (or empty). Consume BOTH dollars so the second one is not
            // re-dispatched into this same branch on the next character.
            index = start + 2
            return text("$$")
        }

        // `$…$` — only when the body is shaped like an expression rather than a price.
        val close = src.indexOf('$', start + 1)
        if (close > start + 1) {
            val body = src.substring(start + 1, close)
            if (looksLikeTex(body)) {
                index = close + 1
                return mathNode(body)
            }
        }

        index = start + 1
        return text("$")
    }

    private fun mathNode(latex: String): Node = JLatexMathNode().apply { latex(latex) }
}

/**
 * Is the text between two single `$` inline math, or is the `$` a currency sign?
 *
 * Ported from HarmonyOS `core/Markdown.ets` so the two hand-written clients agree on the
 * one construct where the answer is a judgement call. The tests, in order:
 *
 *   1. a TeX metacharacter is decisive          -> math
 *   2. a body opening with a digit is a price   -> not math ("$100 and $200" has
 *                                                  body "100 and ")
 *   3. CJK inside means the pair spans prose    -> not math
 *   4. otherwise short and expression-shaped is math, but an interior space is only
 *      allowed alongside an operator, so "five or " stays prose while "x + y" is math
 *
 * Rule 4 also rejects a body containing a newline (a newline is neither space, operator,
 * nor alphanumeric), so a `$` pair does not silently span two lines of prose.
 */
internal fun looksLikeTex(body: String): Boolean {
    for (c in body) {
        if (c == '\\' || c == '^' || c == '_' || c == '{' || c == '}') return true
    }

    val t = body.trim()
    if (t.isEmpty() || t.length > 48) return false
    // A price: "$100", "$1,200.50". Math almost never opens a bare `$…$` with a digit.
    if (t[0] in '0'..'9') return false

    var hasSpace = false
    var hasOperator = false
    for (c in t) {
        val code = c.code
        if (code > 0x2e7f) return false          // CJK (and friends): the $ pair spans prose
        when {
            c == ' ' -> hasSpace = true
            c == '+' || c == '-' || c == '*' || c == '/' || c == '=' ||
                c == '<' || c == '>' || c == '(' || c == ')' || c == ',' ||
                c == '.' || c == '|' || c == '!' -> hasOperator = true
            c in '0'..'9' || c in 'A'..'Z' || c in 'a'..'z' -> Unit
            else -> return false                 // anything else is not expression-shaped
        }
    }
    // Several words with no operator is prose that merely sits between two dollars.
    return !hasSpace || hasOperator
}
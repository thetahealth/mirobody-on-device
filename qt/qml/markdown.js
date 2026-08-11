// Splitting a reply into the pieces that need different renderers, and finding the
// math inside the prose.
//
// A port of android's ui/chat/MarkdownSegments.kt (the fence splitter) and
// ui/chat/InlineMath.kt (`looksLikeTex`), which is itself harmony's
// core/Markdown.ets — the clients agree on the one construct where "is this math or
// is this a dollar sign" is a judgement call, so they must share the rule.
//
// `.pragma library` makes this a shared, stateless module.

.pragma library

//------------------------------------------------------------------------------
// Fenced blocks
//------------------------------------------------------------------------------

// Split a reply into renderable segments: [{ kind, text }] where kind is
// "md" | "svg" | "chart".
//
// STREAMING IS WHAT SHAPES THIS. A message is re-split on every token, so an
// **unclosed** fence is deliberately left as markdown: half an SVG is not a figure
// and half a JSON option is not a chart, and the reader is better served watching
// source arrive than watching a figure flicker in and out. A block becomes a figure
// the moment its closing fence lands, and not before.
//
// Fences that are not ours are still parsed — and skipped over — so a ```svg written
// *inside* a ```markdown example stays example text rather than being lifted out and
// drawn.
function splitMessage(text) {
    var src = String(text || "");
    if (src.indexOf("```") < 0 && src.indexOf("~~~") < 0) {
        return [{ kind: "md", text: src }];
    }

    var out = [];
    var prose = [];
    var lines = src.split("\n");
    var i = 0;

    function flush() {
        if (prose.length > 0) {
            var joined = prose.join("\n");
            if (joined.trim().length > 0) { out.push({ kind: "md", text: joined }); }
            prose = [];
        }
    }

    while (i < lines.length) {
        var open = fenceOpen(lines[i]);
        if (open === null) {
            prose.push(lines[i]);
            i++;
            continue;
        }

        var closed = -1;
        for (var j = i + 1; j < lines.length; ++j) {
            if (fenceCloses(lines[j], open)) { closed = j; break; }
        }
        if (closed < 0) {
            // Still streaming. Keep the rest as text; the figure appears when it closes.
            while (i < lines.length) { prose.push(lines[i]); i++; }
            break;
        }

        var body = lines.slice(i + 1, closed).join("\n");
        if (open.info === "svg") {
            flush();
            out.push({ kind: "svg", text: body });
        } else if (open.info === "echarts") {
            flush();
            out.push({ kind: "chart", text: body });
        } else {
            for (var k = i; k <= closed; ++k) { prose.push(lines[k]); }
        }
        i = closed + 1;
    }

    flush();
    return out.length > 0 ? out : [{ kind: "md", text: src }];
}

// The fence this line opens, or null: { char, count, info }.
//
// Up to three leading spaces, then three or more of the same fence character — a
// fourth space makes it an indented code block instead, and both the character and
// the count matter, which is what lets a ```` block contain a ``` line.
function fenceOpen(line) {
    var i = 0;
    while (i < 3 && i < line.length && line.charAt(i) === " ") { i++; }
    if (i >= line.length) { return null; }
    var c = line.charAt(i);
    if (c !== "`" && c !== "~") { return null; }
    var n = 0;
    while (i + n < line.length && line.charAt(i + n) === c) { n++; }
    if (n < 3) { return null; }
    var info = line.substring(i + n).trim();
    // CommonMark: a backtick fence's info string may not itself contain a backtick.
    if (c === "`" && info.indexOf("`") >= 0) { return null; }
    return { char: c, count: n, info: info.toLowerCase() };
}

// Does this line close `open`? Same character, at least as many, nothing else on it.
function fenceCloses(line, open) {
    var i = 0;
    while (i < 3 && i < line.length && line.charAt(i) === " ") { i++; }
    var n = 0;
    while (i + n < line.length && line.charAt(i + n) === open.char) { n++; }
    if (n < open.count) { return false; }
    return line.substring(i + n).trim().length === 0;
}

//------------------------------------------------------------------------------
// Math
//------------------------------------------------------------------------------

// A placeholder that survives the markdown -> HTML conversion untouched: a bare
// alphanumeric word is not markup in any dialect and nothing escapes it, so it comes
// out of QTextDocument spelled exactly as it went in. The caller swaps each one for
// an <img> once its formula has rendered (or for the original source if it has not).
//
// The suffix is arbitrary noise for one reason: a reply that happened to contain the
// token as prose would have that prose replaced by someone else's formula.
var TOKEN_PREFIX = "mbmath7f3a";

function tokenAt(index) { return TOKEN_PREFIX + index + "z"; }

// The half-open [start, end) ranges of this segment that are CODE, in order.
//
// Two forms, and both have to be found before any math is: a fenced block (the
// splitter leaves every fence that is not ours in place) and an inline span. A `$`
// inside either is a dollar sign in a code sample, never a formula — the help
// document's own Python block is the standing test of that.
//
// Fences are line-bound so this walks lines; inline spans are found within the lines
// that are not inside a fence. Both kinds are recorded as absolute offsets, because
// the math scan below is NOT line-bound: `$$…$$` display math routinely puts its
// delimiters on their own lines.
function codeRegions(src) {
    var regions = [];
    var lines = src.split("\n");
    var pos = 0;
    var fence = null;

    for (var i = 0; i < lines.length; ++i) {
        var line = lines[i];
        var lineStart = pos;
        pos += line.length + 1;              // + the "\n" that split() removed

        if (fence !== null) {
            regions.push([lineStart, lineStart + line.length]);
            if (fenceCloses(line, fence)) { fence = null; }
            continue;
        }
        var open = fenceOpen(line);
        if (open !== null) {
            fence = open;
            regions.push([lineStart, lineStart + line.length]);
            continue;
        }

        var j = 0;
        while (j < line.length) {
            if (line.charAt(j) !== "`") { j++; continue; }
            var n = 0;
            while (j + n < line.length && line.charAt(j + n) === "`") { n++; }
            var ticks = line.substring(j, j + n);
            var close = line.indexOf(ticks, j + n);
            // An unterminated span runs to the end of the line, which is also what a
            // markdown renderer does with it.
            var end = close < 0 ? line.length : close + n;
            regions.push([lineStart + j, lineStart + end]);
            j = end;
        }
    }
    return regions;
}

// Find the math in a markdown segment.
//
// Returns { text, spans } — `text` with every formula replaced by its placeholder,
// and `spans` as [{ token, tex, display, raw }] in the order found.
function scanMath(markdown) {
    var src = String(markdown || "");
    if (src.indexOf("$") < 0) { return { text: src, spans: [] }; }

    var regions = codeRegions(src);
    var r = 0;                               // regions are sorted; walk them alongside
    var out = "";
    var spans = [];
    var i = 0;

    while (i < src.length) {
        while (r < regions.length && regions[r][1] <= i) { r++; }
        if (r < regions.length && regions[r][0] <= i) {
            // Inside code: copy the whole region through untouched.
            var end = regions[r][1];
            out += src.substring(i, end);
            i = end;
            continue;
        }

        if (src.charAt(i) !== "$") {
            out += src.charAt(i);
            i++;
            continue;
        }

        // How far a closing delimiter may be looked for.
        //
        // Android and harmony get this for free: their `$` handler is an inline
        // processor, so the parser only ever hands it ONE paragraph, already stripped
        // of code. Scanning a whole reply as flat text has neither guarantee, and
        // without them a `$` before a price searched forward until it found another
        // one — several sections away, inside a Python block — and `looksLikeTex`
        // said yes on the first `_` it met in the source. The reply then lost
        // everything between the two.
        //
        // So a formula may not reach into code, and (single `$` only) may not cross a
        // blank line: a paragraph break is where the parser those two rely on would
        // have stopped. `$$` is left to span paragraphs, since display math routinely
        // sits on lines of its own.
        var codeLimit = r < regions.length ? regions[r][0] : src.length;

        // --- $$…$$ — unambiguous, no currency reading to guard against ---
        if (src.charAt(i + 1) === "$") {
            var dclose = src.indexOf("$$", i + 2);
            if (dclose > i + 2 && dclose < codeLimit) {
                var token = tokenAt(spans.length);
                spans.push({ token: token, tex: src.substring(i + 2, dclose),
                             display: true, raw: src.substring(i, dclose + 2) });
                out += token;
                i = dclose + 2;
                continue;
            }
            // Unterminated (or empty). Consume BOTH dollars so the second is not
            // re-dispatched into this same branch on the next character — which is
            // also what makes a half-streamed `$$x` render as written instead of
            // eating the rest of the message.
            out += "$$";
            i += 2;
            continue;
        }

        // --- $…$ — only when the body is shaped like an expression -------
        var para = src.indexOf("\n\n", i + 1);
        var limit = Math.min(codeLimit, para < 0 ? src.length : para);
        var sclose = src.indexOf("$", i + 1);
        if (sclose > i + 1 && sclose < limit && looksLikeTex(src.substring(i + 1, sclose))) {
            var tok = tokenAt(spans.length);
            spans.push({ token: tok, tex: src.substring(i + 1, sclose),
                         display: false, raw: src.substring(i, sclose + 1) });
            out += tok;
            i = sclose + 1;
            continue;
        }

        out += "$";
        i++;
    }

    return { text: out, spans: spans };
}

/**
 * Is the text between two single `$` inline math, or is the `$` a currency sign?
 *
 * Ported from android's InlineMath.kt / harmony's core/Markdown.ets, unchanged. The
 * tests, in order:
 *
 *   1. a TeX metacharacter is decisive          -> math
 *   2. a body opening with a digit is a price   -> not math ("$100 and $200" has
 *                                                  body "100 and ")
 *   3. CJK inside means the pair spans prose    -> not math
 *   4. otherwise short and expression-shaped is math, but an interior space is only
 *      allowed alongside an operator, so "five or " stays prose while "x + y" is math
 *
 * Rule 4 also rejects a body containing a newline (a newline is neither space,
 * operator, nor alphanumeric), so a `$` pair does not silently span two lines of prose.
 */
function looksLikeTex(body) {
    for (var i = 0; i < body.length; ++i) {
        var ch = body.charAt(i);
        if (ch === "\\" || ch === "^" || ch === "_" || ch === "{" || ch === "}") {
            return true;
        }
    }

    var t = body.trim();
    if (t.length === 0 || t.length > 48) { return false; }
    // A price: "$100", "$1,200.50". Math almost never opens a bare $…$ with a digit.
    if (t.charAt(0) >= "0" && t.charAt(0) <= "9") { return false; }

    var hasSpace = false;
    var hasOperator = false;
    for (var j = 0; j < t.length; ++j) {
        var c = t.charAt(j);
        var code = t.charCodeAt(j);
        if (code > 0x2e7f) { return false; }   // CJK (and friends): the pair spans prose
        if (c === " ") {
            hasSpace = true;
        } else if ("+-*/=<>(),.|!".indexOf(c) >= 0) {
            hasOperator = true;
        } else if ((c >= "0" && c <= "9") || (c >= "A" && c <= "Z") || (c >= "a" && c <= "z")) {
            // expression-shaped
        } else {
            return false;
        }
    }
    // Several words with no operator is prose that merely sits between two dollars.
    return !hasSpace || hasOperator;
}

//------------------------------------------------------------------------------

// HTML-escape a fallback (the raw `$…$` shown when a formula could not be drawn).
function escapeHtml(s) {
    return String(s)
        .replace(/&/g, "&amp;")
        .replace(/</g, "&lt;")
        .replace(/>/g, "&gt;");
}

// Pretty-print a JSON string for a tool card, or return it unchanged when it is not
// JSON (arguments stream in pieces, so a running call often holds half an object).
function prettyJson(s) {
    try {
        return JSON.stringify(JSON.parse(s), null, 2);
    } catch (e) {
        return String(s || "");
    }
}

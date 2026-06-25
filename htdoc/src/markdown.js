// Renders chat content as Markdown with LaTeX math, mirroring the app's
// MarkdownText (Markwon + JLatexMath): GFM (tables, strikethrough, autolinks)
// plus `$...$` inline and `$$...$$` block math. The model's output is untrusted,
// so the generated HTML is sanitized (DOMPurify) before it ever reaches
// innerHTML -- the CSP is a backstop, not the primary defense.
//
// All deps are bundled (no CDN), so script/style/font all stay same-origin
// under the server's CSP. KaTeX's stylesheet (and its fonts) ride along.

require("katex/dist/katex.min.css");

var _marked = require("marked");
var marked  = _marked.marked || _marked;          // v18 exports { marked, ... }
var katex   = require("katex");
katex = katex.default || katex;
var DOMPurify = require("dompurify");
DOMPurify = DOMPurify.default || DOMPurify;

//----------------------------------------------------------------------------

function renderMath(expr, display) {
    try {
        return katex.renderToString(expr, {
            throwOnError : false,      // partial/streamed math renders, doesn't throw
            displayMode  : !!display,
            output       : "html"
        });
    } catch (e) {
        return null;                   // fall back to the raw source at the call site
    }
};

// `$$...$$` — block math on its own line(s).
var blockMath = {
    name  : "blockMath",
    level : "block",
    start : function (src) { var i = src.indexOf("$$"); return i < 0 ? undefined : i; },
    tokenizer : function (src) {
        var m = /^\$\$([\s\S]+?)\$\$/.exec(src);
        if (m) { return { type: "blockMath", raw: m[0], text: m[1].trim() }; }
    },
    renderer : function (token) {
        var html = renderMath(token.text, true);
        return html != null ? html : token.raw;
    }
};

// `$...$` — inline math. The guards keep "$5 and $6"-style currency from being
// mistaken for math: the opening `$` must be followed by a non-space, the
// closing `$` preceded by a non-space and NOT followed by a digit, and no `$`
// may appear inside.
var inlineMath = {
    name  : "inlineMath",
    level : "inline",
    start : function (src) { var i = src.indexOf("$"); return i < 0 ? undefined : i; },
    tokenizer : function (src) {
        var m = /^\$(?![\s$])([^\n$]*?[^\s$])\$(?!\d)/.exec(src);
        if (m) { return { type: "inlineMath", raw: m[0], text: m[1] }; }
    },
    renderer : function (token) {
        var html = renderMath(token.text, false);
        return html != null ? html : token.raw;
    }
};

// A ```svg fenced block renders as the actual SVG rather than escaped source
// (models often wrap diagrams this way). Raw inline <svg> already passes through
// marked's HTML; both are sanitized by DOMPurify below, which keeps SVG.
var svgFence = {
    name  : "svgFence",
    level : "block",
    start : function (src) { var m = /(^|\n)```[ \t]*svg\b/.exec(src); return m ? m.index : undefined; },
    tokenizer : function (src) {
        var m = /^```[ \t]*svg[ \t]*\r?\n([\s\S]*?)\r?\n```[ \t]*(?:\r?\n|$)/.exec(src);
        if (m) { return { type: "svgFence", raw: m[0], text: m[1] }; }
    },
    renderer : function (token) { return token.text; }   // sanitized downstream
};

marked.use({ gfm: true, breaks: true, extensions: [blockMath, inlineMath, svgFence] });

//----------------------------------------------------------------------------

// Open Markdown links in a new tab, defused against reverse-tabnabbing.
DOMPurify.addHook("afterSanitizeAttributes", function (node) {
    if (node.tagName === "A") {
        node.setAttribute("target", "_blank");
        node.setAttribute("rel", "noopener noreferrer");
    }
});

function escapeHtml(s) {
    return String(s).replace(/[&<>"']/g, function (c) {
        return { "&": "&amp;", "<": "&lt;", ">": "&gt;", '"': "&quot;", "'": "&#39;" }[c];
    });
};

// Markdown (+ math) source -> sanitized HTML string ready for innerHTML.
function render(text) {
    var html;
    try {
        html = marked.parse(text || "");
    } catch (e) {
        return escapeHtml(text || "");
    }
    return DOMPurify.sanitize(html, { ADD_ATTR: ["target"] });
};

exports.render = render;

//----------------------------------------------------------------------------

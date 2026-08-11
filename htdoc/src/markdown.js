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

// A ```echarts fenced block draws as the figure it describes.
//
// A chart normally arrives as a stream event, not as text (docs/markdown.md
// §4.2) -- but the fence is the display form harmony and android give a chart in
// the message body, and it is the only one that puts a figure BETWEEN the
// paragraph that introduces it and the one that reads it. Understanding it here
// is what lets a written figure land where it was written; it is also what makes
// the built-in guide (/help) able to show a chart with no model and no tool call.
//
// The option rides in the placeholder's own data attribute, and charts.hydrate()
// (called right after the sanitized HTML lands) turns it into a canvas -- an
// ECharts option is a live object, not markup, so nothing here can render it.
// A fence whose body doesn't parse falls back to its source: half a JSON option
// is garbage, not a preview, and a message is re-parsed on every streamed token.
var echartsFence = {
    name  : "echartsFence",
    level : "block",
    start : function (src) { var m = /(^|\n)```[ \t]*echarts\b/.exec(src); return m ? m.index : undefined; },
    tokenizer : function (src) {
        var m = /^```[ \t]*echarts[ \t]*\r?\n([\s\S]*?)\r?\n```[ \t]*(?:\r?\n|$)/.exec(src);
        if (m) { return { type: "echartsFence", raw: m[0], text: m[1] }; }
    },
    renderer : function (token) {
        try {
            JSON.parse(token.text);
        } catch (e) {
            return "<pre><code>" + escapeHtml(token.text) + "</code></pre>";
        }
        return '<div class="mb-chart" data-mb-chart="' + escapeHtml(token.text) + '"></div>';
    }
};

marked.use({ gfm: true, breaks: true,
             extensions: [blockMath, inlineMath, svgFence, echartsFence] });

//----------------------------------------------------------------------------

// Only http, https and mailto are handed to the browser -- the same allowlist
// harmony and android apply (docs/markdown.md §5), narrower than DOMPurify's
// default (which also passes ftp, tel, sms, callto, cid and xmpp). A link is text
// a model wrote; which handler the machine launches is not its call. The scheme
// is read off the RESOLVED url, so an ordinary relative link still works and only
// an explicit foreign scheme is refused -- href and all, so the text stays but
// nothing happens on click.
//
// Whatever survives opens in a new tab, defused against reverse-tabnabbing.
function openable(href) {
    try {
        var scheme = new URL(href, window.location.href).protocol;
        return scheme === "http:" || scheme === "https:" || scheme === "mailto:";
    } catch (e) {
        return false;
    }
};

DOMPurify.addHook("afterSanitizeAttributes", function (node) {
    if (node.tagName !== "A") { return; }
    if (!openable(node.getAttribute("href") || "")) {
        node.removeAttribute("href");
        return;
    }
    node.setAttribute("target", "_blank");
    node.setAttribute("rel", "noopener noreferrer");
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

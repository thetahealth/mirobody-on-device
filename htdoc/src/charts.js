
//----------------------------------------------------------------------------
// ECharts: loading the vendored library on demand, drawing one option with the
// app's theme, and hydrating the chart placeholders markdown.js leaves behind.
//
// A chart reaches this client two ways, and they are not the same kind of thing
// (docs/markdown.md §4.2):
//
//   - as a stream EVENT -- the model calls render_chart, the core's ChartFilter
//     lifts the option out, and {"type":"chart"} arrives beside the reply text.
//     chat.js draws it in the visuals block under the answer.
//   - as a ```echarts FENCE in the message text -- the display form harmony and
//     android use to put a figure back between the paragraph that introduces it
//     and the one that reads it. markdown.js turns the fence into a placeholder
//     and hydrate() fills it, so a written figure sits where it was written.
//
// Both end at draw(), so the theme, the option scrubbing and the sizing are
// decided once.
//----------------------------------------------------------------------------

const ui  = require("./ui");
const net = require("./net");

const config = require("./config");
const color  = config.color;

//----------------------------------------------------------------------------

// ECharts is a doc-root static asset (static/echarts.min.js ~1 MB), loaded lazily
// the first time a chart appears so users who never see one don't pay for it up
// front. Mirrors the qrcode/tanka-signer lazy loads (see tanka.js). chart-theme.js
// (the shared mirobody chart theme, another static/ copy) rides along
// best-effort: if it fails to load, charts degrade to stock ECharts instead of
// never appearing.
var echartsPromise = null;

function ensureECharts() {
    if (echartsPromise) { return echartsPromise; }
    function load(src, required) {
        return new Promise(function (resolve, reject) {
            var s = document.createElement("script");
            s.src = net.appBase() + src;
            s.onload = function () { resolve(); };
            s.onerror = required ? reject : function () { resolve(); };
            document.head.appendChild(s);
        });
    }
    // chart-theme.js closes over `globalThis`, which is ES2020 -- while
    // echarts.min.js is transpiled to ES5 and loads anywhere. On an engine that
    // predates it the theme file therefore throws before defining mbChartTheme,
    // and charts silently lose their palette while still drawing. Android hit
    // exactly this in a WebView (docs/markdown.md §4.2); the fix belongs to the
    // host, not the script, because that file is vendored byte-identical to four
    // clients. This is that host.
    if (typeof window.globalThis === "undefined") { window.globalThis = window; }
    echartsPromise = window.echarts
        ? Promise.resolve(window.echarts)
        : Promise.all([load("/echarts.min.js", true), load("/chart-theme.js", false)])
            .then(function () { return window.echarts; });
    return echartsPromise;
};

//----------------------------------------------------------------------------

// Draw one ECharts option into `holder`, which must already have a non-zero size
// (echarts measures its container once at init and no resize event follows a
// late layout). A malformed option leaves the holder empty rather than throwing.
function draw(holder, option) {
    if (!holder || !option || typeof option !== "object" || !Object.keys(option).length) {
        return Promise.resolve(null);
    }
    return ensureECharts().then(function (echarts) {
        if (!echarts) { return null; }
        try {
            // Shared theme: the validated series palette plus the app's own
            // ink/hairline colors as chart chrome; mbPrepare() strips any
            // model-supplied colors so the theme actually applies (and adds a
            // legend when several series must be told apart). Guarded -- if
            // chart-theme.js didn't load, fall back to stock ECharts. Both mode +
            // chrome are read at render time, so a chart picks up the theme in
            // effect when it arrives (older canvases keep theirs until a
            // re-render drops them, like a reload does).
            var theme = window.mbChartTheme ? window.mbChartTheme(config.isDarkTheme(), {
                ink: color.onSurface, inkDim: color.onSurfaceVar,
                axis: color.outline, grid: color.outlineVar,
                surface: "transparent"
            }) : null;
            var chart = echarts.init(holder, theme, { renderer: "canvas" });
            chart.setOption(window.mbPrepare ? window.mbPrepare(option) : option);
            window.addEventListener("resize", function () {
                if (!chart.isDisposed()) { chart.resize(); }
            });
            return chart;
        } catch (e) {
            return null;   // bad option: an empty holder rather than a crash
        }
    }, function () {
        return null;       // echarts failed to load: leave the holder empty
    });
};

// A holder sized for one chart in the reply column. Phones get a shorter box so a
// figure doesn't take the whole screen.
function holder() {
    return ui.dom("div", { width: "100%", height: config.isMobile() ? "240px" : "320px" });
};

//----------------------------------------------------------------------------

// Fill every ```echarts placeholder inside a freshly rendered reply.
//
// The option travels in the placeholder's own data attribute rather than in a
// module-level registry keyed by id: an assistant bubble is re-rendered on every
// streamed token, so anything kept on the side would grow once per parse and
// never be collected.
function hydrate(root) {
    if (!root || !root.querySelectorAll) { return; }
    var nodes = root.querySelectorAll("div.mb-chart[data-mb-chart]");
    for (var i = 0; i < nodes.length; i ++) {
        var node = nodes[i];
        var option = null;
        try { option = JSON.parse(node.getAttribute("data-mb-chart")); } catch (e) { option = null; }
        // Consumed either way: an unparseable option must not be retried on the
        // next hydrate() of the same node.
        node.removeAttribute("data-mb-chart");
        if (!option) { continue; }
        ui.setStyle(node, { width: "100%", height: config.isMobile() ? "240px" : "320px" });
        draw(node, option);
    }
};

// Dispose the charts inside a node about to have its innerHTML replaced.
//
// echarts keeps every live instance in a registry keyed by its DOM node, so a
// canvas whose node is dropped by a re-render stays reachable -- and a streaming
// reply re-renders ~10x a second. Nothing to do before the library has loaded:
// there can be no instance yet.
function disposeIn(root) {
    if (!root || !root.querySelectorAll || !window.echarts) { return; }
    var nodes = root.querySelectorAll("div.mb-chart");
    for (var i = 0; i < nodes.length; i ++) {
        var chart = window.echarts.getInstanceByDom(nodes[i]);
        if (chart) { chart.dispose(); }
    }
};

//----------------------------------------------------------------------------

exports.ensureECharts = ensureECharts;
exports.draw          = draw;
exports.holder        = holder;
exports.hydrate       = hydrate;
exports.disposeIn     = disposeIn;

//----------------------------------------------------------------------------

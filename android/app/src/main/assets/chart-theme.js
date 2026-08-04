// Shared ECharts theming for every mirobody client that renders a chart.
//
// SOURCE OF TRUTH: this file. Vendored copies (keep byte-identical):
//   android/app/src/main/assets/chart-theme.js
//   ios/Mirobody/Resources/chart-theme.js
// Same convention as echarts.min.js, which each client bundles its own copy of.
//
// Two entry points:
//   mbChartTheme(dark, palette) -> theme name to pass to echarts.init()
//   mbPrepare(option)           -> the option, made safe for that theme
//
// `palette` carries the HOST's resolved colors ({ink, inkDim, axis, grid, surface}),
// because each client owns its own light/dark scheme — the chart borrows the app's
// text and hairline colors instead of inventing its own. `surface` is only used for
// the 2px gap between adjacent fills; pass 'transparent' when the chart is a live
// canvas over the app's own background, or the opaque bubble color when the result
// is rasterized (harmony bakes a PNG, which has no alpha to blend with).

(function (g) {
    'use strict';

    // Fixed-order categorical palette: the same eight hues, stepped per mode.
    // Validated as a set against each mode's surface — dark passes every gate;
    // light passes with a contrast WARN on 4 of 8 slots (orange/aqua/yellow/magenta
    // fall under 3:1), which is why mbPrepare() forces a legend. Assign slots in
    // order and never cycle: past 8 series, fold the rest into "Other".
    var LIGHT_SERIES = ['#2a78d6', '#eb6834', '#1baf7a', '#eda100',
                        '#e87ba4', '#008300', '#4a3aa7', '#e34948'];
    var DARK_SERIES  = ['#3987e5', '#d95926', '#199e70', '#c98500',
                        '#d55181', '#008300', '#9085e9', '#e66767'];

    function makeTheme(p) {
        var axis = {
            axisLine: { lineStyle: { color: p.axis } },
            axisTick: { lineStyle: { color: p.axis } },
            axisLabel: { color: p.inkDim },
            splitLine: { lineStyle: { color: [p.grid] } },
            splitArea: { show: false }
        };
        return {
            color: p.series,
            backgroundColor: 'transparent',
            textStyle: { color: p.ink },
            title: { textStyle: { color: p.ink }, subtextStyle: { color: p.inkDim } },
            legend: { textStyle: { color: p.inkDim } },
            categoryAxis: axis, valueAxis: axis, logAxis: axis, timeAxis: axis,
            // Thin marks; a 2px surface-colored gap between adjacent/stacked fills.
            line: { lineStyle: { width: 2 }, symbolSize: 8 },
            bar: { itemStyle: { borderRadius: [4, 4, 0, 0], borderColor: p.surface, borderWidth: 2 } },
            pie: { itemStyle: { borderColor: p.surface, borderWidth: 2 } }
        };
    }

    // Registering the same name twice is harmless but pointless; the palette a host
    // passes never changes within one mode, so the first registration wins.
    var registered = {};

    g.mbChartTheme = function (dark, palette) {
        var name = dark ? 'mb-dark' : 'mb-light';
        if (!registered[name]) {
            registered[name] = true;
            echarts.registerTheme(name, makeTheme({
                series: dark ? DARK_SERIES : LIGHT_SERIES,
                ink: palette.ink,
                inkDim: palette.inkDim,
                axis: palette.axis,
                grid: palette.grid,
                surface: palette.surface
            }));
        }
        return name;
    };

    // In ECharts the OPTION beats the THEME, so one stray "color" from the model
    // repaints the chart for whichever mode it guessed and breaks the other.
    // res/mcp_tools/render_chart.cpp tells the model to stay theme-neutral; this
    // enforces it, because an option can also arrive as a hand-written ```echarts
    // fence that never passed through that tool.
    //
    // visualMap is exempt: its color is a data encoding (a sequential ramp), not
    // chrome — dropping it would delete the chart's meaning. Such a ramp is authored
    // blind to the mode and may read poorly on the opposite one; that is the known
    // cost of keeping it.
    function deTheme(node, inVisualMap) {
        if (node === null || typeof node !== 'object') { return; }
        if (Array.isArray(node)) {
            for (var i = 0; i < node.length; i++) { deTheme(node[i], inVisualMap); }
            return;
        }
        for (var k in node) {
            if (!Object.prototype.hasOwnProperty.call(node, k)) { continue; }
            var visual = inVisualMap || k === 'visualMap';
            if (!visual && (k === 'color' || k === 'backgroundColor' ||
                            k === 'borderColor' || k === 'shadowColor')) {
                delete node[k];
                continue;
            }
            deTheme(node[k], visual);
        }
    }

    // Relief for the light palette's sub-3:1 slots: identity must not rest on color
    // alone. ECharts builds the entries from the series names, so an empty object is
    // enough — but only if the model named its series.
    function ensureLegend(option) {
        var n = Array.isArray(option.series) ? option.series.length : 0;
        if (n >= 2 && option.legend === undefined) { option.legend = {}; }
    }

    g.mbPrepare = function (option) {
        deTheme(option, false);
        ensureLegend(option);
        return option;
    };
// globalThis, not top-level `this`: the latter is the global only for a classic
// browser script (which is how all three hosts load this), but is module.exports
// under Node and undefined in a module — and this file is unit-tested under Node.
})(globalThis);

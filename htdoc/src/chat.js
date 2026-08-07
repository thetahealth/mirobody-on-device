
//----------------------------------------------------------------------------
// The chat view: the provider picker in the top bar, the scrolling message log
// (user bubbles + rendered-Markdown assistant turns, each with a stats/copy
// footer), and the composer. Runs the selected agent ("Agent/provider") and
// streams its {type, content} events (reply / thinking / cost / error) as SSE.
//----------------------------------------------------------------------------

const ui   = require("./ui");
const net  = require("./net");
const db   = require("./db");
const i18n = require("./i18n");
const app  = require("./app");

const config = require("./config");
const color  = config.color;
const serifFamily = config.serifFamily;
const state  = config.state;
const isMobile    = config.isMobile;
const isAndroid   = config.isAndroid;
const PROVIDER_KEY = config.PROVIDER_KEY;

const widgets = require("./widgets");
const renderInto   = widgets.renderInto;
const streamRender = widgets.streamRender;
const button       = widgets.button;

const icons = require("./icons");
const CARET_SVG  = icons.CARET_SVG;
const DOLLAR_SVG = icons.DOLLAR_SVG;
const COPY_SVG   = icons.COPY_SVG;
const ATTACH_SVG = icons.ATTACH_SVG;
const FILE_SVG   = icons.FILE_SVG;
const CLOSE_SVG  = icons.CLOSE_SVG;
const INCOGNITO_SVG  = icons.INCOGNITO_SVG;
const INCOGNITO_OUTLINE_SVG = icons.INCOGNITO_OUTLINE_SVG;
const SEND_ARROW_SVG = icons.SEND_ARROW_SVG;

const formatLocalTime = require("./format").formatLocalTime;
const showCostModal   = require("./modals").showCostModal;
const showConfirmModal = require("./modals").showConfirmModal;

var t = i18n.t;

//----------------------------------------------------------------------------
// On-device provider (desktop only). The Electron preload exposes window.ondevice
// (see electron/preload.js + electron/ondevice.js); a plain browser has no bridge,
// so the on-device option never appears there. Name/code match the other clients.

// Each registered on-device model shows in the picker as "<name> · On-device".
var ONDEVICE_SUFFIX = " · On-device";
var ONDEVICE_MANAGE = "__ondevice_manage__";   // sentinel picker option that opens the manager
var onDeviceModels = [];                         // [{name,status,remote}] cached from the bridge

function onDeviceBridge() {
    return (typeof window !== "undefined") ? window.ondevice : null;
}
function isOnDeviceProvider(name) {
    return !!onDeviceBridge() && typeof name === "string"
        && name.length > ONDEVICE_SUFFIX.length
        && name.slice(-ONDEVICE_SUFFIX.length) === ONDEVICE_SUFFIX;
}
function modelFromProvider(name) { return name.slice(0, -ONDEVICE_SUFFIX.length); }

// Refresh the cached on-device model list from the bridge, then rebuild the provider
// picker (one entry per model) and run an optional callback.
function refreshOnDeviceModels(then) {
    var b = onDeviceBridge();
    if (!b) { if (then) { then(); } return; }
    b.models().then(function (m) {
        onDeviceModels = (m instanceof Array) ? m : [];
        if (app.slots && app.slots.providerRefill) { app.slots.providerRefill(state.providers); }
        if (then) { then(); }
    });
}

// On-device model manager: a list of GGUF models (remote download or local file),
// a curated "suggested" picker, and free-form add-by-URL / add-local-file. Mirrors
// the Qt client's ⚙ → On-device AI dialog. Reuses the overlay+card pattern.
function showOnDeviceManager() {
    var bridge = onDeviceBridge();
    if (!bridge) { return; }

    // Uniform control metrics so the suggested <select>, the text inputs and the
    // buttons all share one height and read as one row (native selects/inputs
    // otherwise render shorter than button()).
    var CTRL_H = "40px";
    function ctrlStyle(extra) {
        var s = {
            height: CTRL_H, padding: "0 10px",
            border: "1px solid " + color.outlineVar, borderRadius: "10px",
            font: "inherit", fontSize: "0.875rem",
            color: color.onSurface, background: color.background,
            boxSizing: "border-box"
        };
        for (var k in extra) { s[k] = extra[k]; }
        return s;
    }
    // A button() sized to CTRL_H (button()'s own padding drives height otherwise,
    // which drifts a pixel or two from the inputs).
    function sizedBtn(label, opts) {
        var b = button(label, false, opts);
        ui.setStyle(b, { height: CTRL_H, padding: "0 16px" });
        if (opts && opts.minWidth) { ui.setStyle(b, { minWidth: opts.minWidth }); }
        return b;
    }

    var backdrop = ui.dom("div", {
        position: "fixed", inset: "0", background: "rgba(0, 0, 0, 0.4)",
        display: "flex", alignItems: "center", justifyContent: "center",
        padding: "16px", zIndex: "1000"
    });
    var card = ui.dom("div", {
        background: color.background, borderRadius: "14px", padding: "20px 22px",
        width: "100%", maxWidth: "480px",
        boxShadow: "0 8px 32px rgba(0, 0, 0, 0.25)",
        display: "flex", flexDirection: "column", gap: "10px"
    });
    card.appendChild(widgets.modalHeader(t("onDeviceAi"), function () { dismiss(); }));
    card.appendChild(ui.setText(ui.dom("div", {
        fontSize: "0.85rem", color: color.onSurfaceVar, lineHeight: "1.4"
    }), t("onDeviceIntro")));

    var listWrap = ui.dom("div", {
        display: "flex", flexDirection: "column", gap: "6px",
        maxHeight: "240px", overflowY: "auto"
    });
    card.appendChild(listWrap);
    card.appendChild(ui.dom("div", { height: "1px", background: color.outlineVar }));

    // Suggested (curated) dropdown + Add.
    var sugRow = ui.dom("div", { display: "flex", gap: "6px", alignItems: "center" });
    sugRow.appendChild(ui.setText(ui.dom("span", { fontSize: "0.85rem", color: color.onSurfaceVar }), t("suggested")));
    var sugSelect = ui.dom("select", ctrlStyle({ flex: "1 1 auto", minWidth: "0" }));
    sugRow.appendChild(sugSelect);
    sugRow.appendChild(sizedBtn(t("add"), { click: function () {
        var opt = sugSelect.options[sugSelect.selectedIndex];
        if (opt && opt.__uri) { bridge.addRemote(opt.__name, opt.__uri); refreshOnDeviceModels(renderAll); }
    } }));
    card.appendChild(sugRow);

    // Free-form add-by-URL: name + url.
    var remoteRow = ui.dom("div", { display: "flex", gap: "6px", alignItems: "center" });
    var nameIn = ui.dom("input", ctrlStyle({ width: "96px" }), { type: "text", placeholder: t("modelName") });
    var urlIn = ui.dom("input", ctrlStyle({ flex: "1 1 auto", minWidth: "0" }), { type: "text", placeholder: "https://…/model.gguf" });
    remoteRow.appendChild(nameIn); remoteRow.appendChild(urlIn);
    remoteRow.appendChild(sizedBtn(t("add"), { click: function () {
        if (urlIn.value.trim()) {
            bridge.addRemote(nameIn.value, urlIn.value);
            nameIn.value = ""; urlIn.value = "";
            refreshOnDeviceModels(renderAll);
        }
    } }));
    card.appendChild(remoteRow);

    // Add a local file (native picker in main).
    var localBtn = sizedBtn(t("addLocalFile"), { click: function () {
        bridge.addLocal().then(function (ok) { if (ok) { refreshOnDeviceModels(renderAll); } });
    } });
    ui.setStyle(localBtn, { width: "100%" });
    card.appendChild(localBtn);

    var dlProg = {};   // name -> latest download fraction
    var unsub = bridge.onDownloadProgress(function (p) {
        if (!p || !p.name) { return; }
        if (p.status === "downloading" && typeof p.progress === "number") { dlProg[p.name] = p.progress; }
        else { delete dlProg[p.name]; }
        refreshOnDeviceModels(renderAll);
    });
    function dismiss() {
        if (unsub) { unsub(); unsub = null; }
        if (backdrop.parentNode) { backdrop.parentNode.removeChild(backdrop); }
    }

    function renderList() {
        ui.clear(listWrap);
        onDeviceModels.forEach(function (m) {
            var row = ui.dom("div", { display: "flex", alignItems: "center", gap: "8px" });
            var info = ui.dom("div", { flex: "1 1 auto", minWidth: "0" });
            info.appendChild(ui.setText(ui.dom("div", {
                color: color.onSurface, whiteSpace: "nowrap", overflow: "hidden", textOverflow: "ellipsis"
            }), m.name));
            var st;
            if (m.status === "ready") { st = t("modelReady"); }
            else if (m.status === "downloading") {
                st = t("modelDownloading") + (dlProg[m.name] != null ? " " + Math.round(dlProg[m.name] * 100) + "%" : "");
            } else { st = m.remote ? t("modelNotDownloaded") : t("modelFileMissing"); }
            info.appendChild(ui.setText(ui.dom("div", { fontSize: "0.75rem", color: color.onSurfaceVar }), st));
            row.appendChild(info);
            // Download and Cancel share a min width so the two states line up
            // across rows; ✕ is a fixed square.
            if (m.remote && m.status === "absent") {
                row.appendChild(sizedBtn(t("download"), { minWidth: "104px", click: function () { bridge.download(m.name); } }));
            }
            if (m.status === "downloading") {
                row.appendChild(sizedBtn(t("cancel"), { minWidth: "104px", click: function () { bridge.cancelDownload(); } }));
            }
            // Deleting is destructive (a remote model's file is removed from disk;
            // a local one is only forgotten) — red button + a confirm dialog first.
            var rm = sizedBtn("✕", { click: function () {
                showConfirmModal(
                    t("deleteModel"),
                    t(m.remote ? "deleteModelConfirm" : "forgetModelConfirm", m.name),
                    t("delete"), true,
                    function () { bridge.remove(m.name); refreshOnDeviceModels(renderAll); }
                );
            } });
            ui.setStyle(rm, { width: CTRL_H, minWidth: "0", padding: "0",
                              color: color.error, borderColor: color.error });
            row.appendChild(rm);
            listWrap.appendChild(row);
        });
    }
    function renderSuggest() {
        bridge.suggestions().then(function (sugs) {
            ui.clear(sugSelect);
            (sugs || []).forEach(function (s) {
                var o = ui.dom("option", null, { value: s.name });
                o.__uri = s.uri; o.__name = s.name;
                ui.setText(o, s.label);
                sugSelect.appendChild(o);
            });
            ui.setStyle(sugRow, { display: (sugs && sugs.length) ? "flex" : "none" });
        });
    }
    function renderAll() { renderList(); renderSuggest(); }

    refreshOnDeviceModels(renderAll);
    backdrop.addEventListener("click", function (evt) { if (evt.target === backdrop) { dismiss(); } });
    backdrop.appendChild(card);
    document.body.appendChild(backdrop);
}

//----------------------------------------------------------------------------
// Incognito ("privacy mode"). Toggling swaps the entire chat session: entering
// stashes the real conversation and starts a blank ephemeral one; leaving
// restores the stash and discards the ephemeral turns. While on, buildChat's
// submit() skips the IndexedDB mirror and each request carries `incognito:true`
// so the server persists nothing and disables memory. The app re-renders so the
// banner, the toggle's active tint, and the gating all follow. Never toggled
// mid-stream (the button is inert then) so no in-flight bubble is torn down.
function toggleIncognito() {
    if (state.streaming) { return; }
    if (!state.incognito) {
        state.incognitoSaved = {
            messages : state.messages,
            conv     : state.currentConversationId,
            readOnly : state.readOnly
        };
        state.messages = [];
        state.currentConversationId = "";
        state.readOnly = false;
        state.incognito = true;
    } else {
        var saved = state.incognitoSaved || { messages: [], conv: "", readOnly: false };
        state.messages = saved.messages || [];
        state.currentConversationId = saved.conv || "";
        state.readOnly = !!saved.readOnly;
        state.incognitoSaved = null;
        state.incognito = false;
    }
    app.render();
}

//----------------------------------------------------------------------------

// A borderless <select> styled as a centered text button with a trailing caret
// overlay (appearance:none hides the native arrow). Shared by the composer's
// model picker and the subject picker so both dropdowns read identically.
// `maxWidth` caps the control; returns { wrap, select }.
//
// `surface` is the color of whatever the control sits on. It looks like a no-op
// (the same color as the parent, so the control still reads as borderless), but
// the option list is not ours to style: the browser paints the popup from the
// select's own background-color, and a transparent one leaves it the light
// default -- in dark mode that is our pale text on a pale popup. Passing the
// real surface keeps the popup in the theme. Defaults to the composer bar.
function caretSelect(maxWidth, surface) {
    var select = ui.dom("select", {
        appearance       : "none",
        WebkitAppearance : "none",
        MozAppearance    : "none",
        paddingBlock       : "6px",
        paddingInlineStart : "10px",
        paddingInlineEnd   : "28px",   // room for the caret on the trailing edge
        border           : "none",
        borderRadius     : "10px",
        font             : "inherit",
        fontSize         : "0.875rem",
        fontWeight       : "500",
        color            : color.onSurface,
        background       : surface || color.surfaceLow,
        outline          : "none",
        cursor           : "pointer",
        maxWidth         : maxWidth,
        textAlignLast    : "center"
    });
    var wrap = ui.dom("div", {
        position: "relative", display: "inline-flex", alignItems: "center",
        maxWidth: "100%", borderRadius: "10px"
    });
    wrap.appendChild(select);
    var car = ui.dom("span", {
        position: "absolute", insetInlineEnd: "6px", display: "flex",
        pointerEvents: "none", color: color.onSurfaceVar
    });
    ui.setHTML(car, CARET_SVG);
    wrap.appendChild(car);
    return { wrap: wrap, select: select };
}

//----------------------------------------------------------------------------

function buildChat() {
    var wrap = ui.dom("section", {
        flex          : "1 1 auto",
        display       : "flex",
        flexDirection : "column",
        width         : "100%",   // full width; messages/input fill the panel
        minHeight     : "0"
    });

    //----------------------------------------------------
    // Provider selector. Populated up front from /api/providers so the user
    // picks an agent/provider before sending.

    // Model picker: a borderless caret select (shared style; see caretSelect),
    // placed in the composer's bottom control row (like the reference's "Fable 5").
    var _model = caretSelect("55vw");
    var providerWrap   = _model.wrap;
    var providerSelect = _model.select;
    providerSelect.addEventListener("change", function () {
        // The "Manage models…" sentinel isn't a real selection: open the manager
        // and revert the picker to the previously chosen provider.
        if (providerSelect.value === ONDEVICE_MANAGE) {
            providerSelect.value = state.provider || "";
            showOnDeviceManager();
            return;
        }
        state.provider = providerSelect.value;
        if (state.provider) {
            localStorage.setItem(PROVIDER_KEY, state.provider);
        } else {
            localStorage.removeItem(PROVIDER_KEY);
        }
        // Picking an on-device model that isn't downloaded yet → open the manager.
        if (isOnDeviceProvider(state.provider)) {
            var model = modelFromProvider(state.provider);
            onDeviceBridge().isReady(model).then(function (ready) {
                if (!ready) { showOnDeviceManager(); }
            });
        }
    });

    // provider(model) -> agent name ("" for the default agent), from the grouped
    // /api/providers response; read at submit to send {agent, provider} verbatim.
    var providerAgent = {};

    function setProviderOptions(groups) {
        ui.clear(providerSelect);
        providerAgent = {};

        var names = [];
        for (var i = 0; i < groups.length; i ++) {
            var g = groups[i];
            if (!g || !(g.providers instanceof Array)) { continue; }
            var agent = g.agent || "";
            for (var j = 0; j < g.providers.length; j ++) {
                var p = g.providers[j];
                if (!p || isOnDeviceProvider(p)) { continue; }   // on-device re-added below
                providerAgent[p] = agent;
                names.push(p);
                var opt = ui.dom("option", null, { value: p });
                ui.setText(opt, p);
                providerSelect.appendChild(opt);
            }
        }

        // Desktop only: one entry per *downloaded* on-device model (runs locally,
        // offline, no key), plus a sentinel that opens the model manager. Models
        // that aren't ready yet are reached via the manager, not the picker.
        if (onDeviceBridge()) {
            for (var k = 0; k < onDeviceModels.length; k ++) {
                if (onDeviceModels[k].status !== "ready") { continue; }
                var od = onDeviceModels[k].name + ONDEVICE_SUFFIX;
                names.push(od);
                var odOpt = ui.dom("option", null, { value: od });
                ui.setText(odOpt, od);
                providerSelect.appendChild(odOpt);
            }
            var mng = ui.dom("option", null, { value: ONDEVICE_MANAGE });
            ui.setText(mng, t("manageModels"));
            providerSelect.appendChild(mng);
        }

        if (names.length === 0) {
            // Nothing fetched yet: show a disabled placeholder (the app's
            // "Select model" affordance) without clobbering the cached choice.
            var ph = ui.dom("option", null, { value: "", disabled: "disabled" });
            ui.setText(ph, t("selectModel"));
            providerSelect.appendChild(ph);
            providerSelect.value = "";
            return;
        }

        // Restore the cached selection if it's still on offer; otherwise default
        // to the first provider and persist it so the choice sticks.
        if (names.indexOf(state.provider) < 0) {
            state.provider = names[0];
            localStorage.setItem(PROVIDER_KEY, state.provider);
        }
        providerSelect.value = state.provider;
    };

    // Fill from whatever was already fetched at page load, and expose this
    // selector's refill so a later/in-flight /api/providers response updates it.
    setProviderOptions(state.providers);
    app.slots.providerRefill = setProviderOptions;
    // Desktop: pull the on-device model list from the bridge, then rebuild the
    // picker so each registered model shows as its own "<name> · On-device" option.
    refreshOnDeviceModels();

    // The top bar has no right-hand slot: app settings (language / font / appearance
    // / backend) sit in the left nav drawer alongside the session-scoped items
    // (history, health connections, account, incognito), so one hamburger opens
    // everything -- see history.js.

    //----------------------------------------------------

    // Readable centered column: messages are capped to THREAD_MAX and centered so
    // they don't sprawl edge-to-edge on wide screens. The composer and its
    // attachment-chip row take the same cap, so the input sits exactly under the
    // conversation it belongs to -- one column down the middle of the page, not a
    // wide bar under a narrow thread.
    var THREAD_MAX = "760px";

    var log = ui.dom("div", {
        flex          : "1 1 auto",
        // min-height:0 lets this flex child shrink below its content height so it
        // scrolls internally; without it the default min-height:auto makes the
        // log grow to fit every message and push the input box off-screen.
        minHeight     : "0",
        overflowY     : "auto",
        padding       : "16px",
        display       : "flex",
        flexDirection : "column"
    });
    // Holds every row (timestamps, turns, footers, the empty state). flex:1 lets
    // the empty-state box still center vertically; rows stack from the top.
    var thread = ui.dom("div", {
        flex          : "1 1 auto",
        width         : "100%",
        maxWidth      : THREAD_MAX,
        marginInline  : "auto",
        display       : "flex",
        flexDirection : "column"
    });
    log.appendChild(thread);

    // Auto-follow the streaming reply only while the user is parked at the bottom.
    // A manual scroll up detaches the follow (so earlier text can be read mid-reply
    // without being yanked back down); returning to the bottom re-arms it. Every
    // streaming/progress update calls followBottom() instead of forcing scrollTop,
    // so it never fights the user's scroll. A new turn (appendMessage) re-arms it.
    var stickBottom = true;
    log.addEventListener("scroll", function () {
        stickBottom = log.scrollHeight - log.scrollTop - log.clientHeight < 80;
    });
    function followBottom() {
        if (stickBottom) { log.scrollTop = log.scrollHeight; }
    }

    //----------------------------------------------------

    // Matches the app's MessageRow: the user turn is a dark bubble with an
    // asymmetric tail; the assistant turn is plain text on the background, no
    // bubble or border. Returns the bubble node so the caller can stream into it.
    //
    // A user turn carries its timestamp right under the bubble (right-aligned,
    // inline-end) when `ts` (epoch ms) is given -- so history shows when each
    // exchange happened without cluttering every turn.
    function appendMessage(role, text, ts) {
        var isUser = role === "user";

        var row = ui.dom("div", {
            display        : "flex",
            justifyContent : isUser ? "flex-end" : "flex-start"
        });

        var bubble;
        if (isUser) {
            bubble = ui.setText(ui.dom("div", {
                whiteSpace   : "pre-wrap",
                wordWrap     : "break-word",
                padding      : "5px 9px",
                // Sharp "tail" at the top-inline-end corner (top-right in LTR,
                // top-left in RTL); the other three corners stay rounded.
                borderStartStartRadius : "8px",
                borderStartEndRadius   : "2px",
                borderEndEndRadius     : "8px",
                borderEndStartRadius   : "8px",
                fontSize     : "1rem",
                lineHeight   : "1.75",
                maxWidth     : isMobile() ? "85%" : "320px",
                background   : color.brand,
                color        : color.onPrimary,
                margin       : "32px 0 0 0"
            }), text);
        } else {
            // Assistant turn: rendered Markdown + math (class "md", styled in
            // index.css). No pre-wrap -- the block elements own their spacing.
            bubble = ui.dom("div", {
                wordWrap   : "break-word",
                fontSize   : "1rem",
                lineHeight : "1.6",
                width      : "100%",
                // Flex items default to min-width:auto, which refuses to shrink
                // below the intrinsic width of wide children (code blocks, tables).
                // That would push the row past the viewport and give phones a
                // horizontal scrollbar; min-width:0 lets the bubble shrink so those
                // children honor their own overflow-x:auto (see index.css) instead.
                minWidth   : "0",
                color      : color.onSurface
            });
            bubble.className = "md";
            renderInto(bubble, text);
        }

        row.appendChild(bubble);
        thread.appendChild(row);

        // User turn: timestamp right under the bubble, right-aligned. The -8px
        // top margin tucks it closer to the bubble (offsetting the thread gap).
        if (isUser && ts) {
            var stamp = ui.dom("div", {
                display        : "flex",
                justifyContent : "flex-end",
                margin         : "4px 0"
            });
            stamp.appendChild(ui.setText(ui.dom("div", {
                fontSize : "0.7rem",
                color    : color.onSurfaceVar,
                opacity  : "0.7"
            }), formatLocalTime(ts)));
            thread.appendChild(stamp);
        }

        // A new turn snaps to the bottom and re-arms tail-follow.
        stickBottom = true;
        log.scrollTop = log.scrollHeight;
        return bubble;
    };

    // The "waiting for the model" indicator: three pulsing dots that stand in the
    // assistant bubble from the moment a turn is sent until its first token
    // arrives. The send button turns into a spinner at the same moment, but that
    // is at the far end of the page from where the answer will appear -- and the
    // wait is seconds long whenever the model thinks (or calls a tool) before
    // answering, which is exactly when the user is watching this spot. Sized to
    // roughly one line of body text so the swap to real content doesn't jump.
    // The mb-dot keyframes live in index.css -- the CSP blocks injected <style>,
    // inline styles may only reference them.
    function typingDots() {
        var row = ui.dom("div", {
            display    : "flex",
            alignItems : "center",
            gap        : "4px",
            height     : "1.6rem"
        });
        for (var i = 0; i < 3; i ++) {
            row.appendChild(ui.dom("span", {
                display        : "inline-block",
                width          : "6px",
                height         : "6px",
                borderRadius   : "50%",
                background     : color.onSurfaceVar,
                animation      : "mb-dot 1.2s ease-in-out infinite",
                animationDelay : (i * 0.16) + "s"
            }));
        }
        return row;
    };

    function iconButton(svg, title) {
        var b = ui.dom("button", {
            border         : "none",
            background     : "transparent",
            padding        : "0",
            cursor         : "pointer",
            color          : color.onSurfaceVar,
            display        : "flex",
            alignItems     : "center",
            justifyContent : "center",
            width          : "32px",
            height         : "32px",
            lineHeight     : "0"
        }, { type: "button", title: title });
        ui.setHTML(b, svg);
        return b;
    };

    // Footer beneath an assistant reply: provider label on the left; on the
    // right a stats ($) icon (only when cost data is present) that opens the cost
    // modal, plus a copy icon. Shared by the live stream and history restore so
    // restored replies show the same stats icon as freshly streamed ones.
    function appendFooter(providerLabel, replyText, cost) {
        var label = ui.setText(ui.dom("div", {
            flex       : "1 1 auto",
            display    : "flex",
            alignItems : "center",
            fontSize   : "0.7rem",
            fontWeight : "400",
            color      : color.onSurfaceVar
        }), providerLabel || "");

        var actions = ui.dom("div", {
            flex           : "1 1 auto",
            display        : "flex",
            alignItems     : "center",
            justifyContent : "flex-end"
        });

        if (cost) {
            var dollar = iconButton(DOLLAR_SVG, t("statsTitle"));
            dollar.addEventListener("click", function () { showCostModal(cost); });
            actions.appendChild(dollar);
        }

        var copy = iconButton(COPY_SVG, t("copyReply"));
        copy.addEventListener("click", function () {
            if (navigator.clipboard && navigator.clipboard.writeText) {
                navigator.clipboard.writeText(replyText || "");
            }
        });
        actions.appendChild(copy);

        var row = ui.dom("div", {
            display        : "flex",
            alignItems     : "center",
            justifyContent : "space-between"
        });
        row.appendChild(label);
        row.appendChild(actions);

        thread.appendChild(row);
        followBottom();
        return row;
    };

    function showEmpty() {
        var box = ui.dom("div", {
            margin    : "auto",
            textAlign : "center",
            maxWidth  : "640px",
            width     : "100%",
            boxSizing : "border-box",   // keep the 24px padding inside the width (no h-scroll on phones)
            display       : "flex",
            flexDirection : "column",
            alignItems    : "center",
            gap           : "22px",
            padding       : "24px"
        });
        // Incognito empty state: ghost + "You're incognito" + the not-saved note,
        // mirroring the app's privacy screen. Otherwise the plain serif heading.
        if (state.incognito) {
            box.appendChild(ui.setHTML(ui.dom("div", {
                color: color.primary, lineHeight: "0", fontSize: "0"
            }), incognitoGlyph()));
            box.appendChild(ui.setText(ui.dom("h2", {
                fontFamily : serifFamily,
                fontWeight : "600",
                fontSize   : "clamp(1.6rem, 4vw, 2.1rem)",
                color      : color.onSurface,
                margin     : "0"
            }), t("incognitoHeading")));
            box.appendChild(ui.setText(ui.dom("div", {
                fontSize : "0.9rem",
                color    : color.onSurfaceVar,
                maxWidth : "360px",
                lineHeight : "1.5"
            }), t("incognitoNote")));
        } else {
            // Serif heading (Theta Health style).
            box.appendChild(ui.setText(ui.dom("h2", {
                fontFamily : serifFamily,
                fontWeight : "600",
                fontSize   : "clamp(1.6rem, 4vw, 2.1rem)",
                color      : color.onSurface,
                margin     : "0"
            }), t("chatStart")));
        }

        thread.appendChild(box);
    };

    // A larger ghost for the empty-state hero (the top-bar toggle uses the 22px
    // version); scaled up via width/height on the same markup.
    function incognitoGlyph() {
        return INCOGNITO_SVG.replace('width="22" height="22"', 'width="56" height="56"');
    };

    // A slim incognito banner pinned above the thread when an incognito session
    // already has messages (the empty state carries its own hero instead).
    function incognitoBanner() {
        var bar = ui.dom("div", {
            display        : "flex",
            alignItems     : "center",
            justifyContent : "center",
            gap            : "8px",
            margin         : "0 0 8px",
            padding        : "6px 12px",
            borderRadius   : "10px",
            background     : color.surfaceLow,
            border         : "1px solid " + color.outlineVar,
            color          : color.onSurfaceVar,
            fontSize       : "0.8rem",
            boxSizing      : "border-box",   // padding+border inside the stretched width (no h-scroll)
            maxWidth       : "100%"
        });
        bar.appendChild(ui.setHTML(ui.dom("span", {
            display: "flex", lineHeight: "0", color: color.primary
        }), INCOGNITO_SVG.replace('width="22" height="22"', 'width="16" height="16"')));
        bar.appendChild(ui.setText(ui.dom("span", {}), t("incognitoNote")));
        return bar;
    };

    if (state.incognito && state.messages.length > 0) {
        thread.appendChild(incognitoBanner());
    }

    var i;
    for (i = 0; i < state.messages.length; i ++) {
        var hist = state.messages[i];
        appendMessage(hist.role, hist.content, hist.ts);
        // Rebuild the reply footer (stats + copy) from what was persisted, so a
        // restored answer keeps its stats icon. Messages saved before cost/provider
        // were added have neither -> no footer.
        if (hist.role === "assistant" && (hist.cost || hist.provider)) {
            appendFooter(hist.provider, hist.content, hist.cost || null);
        }
    }
    if (state.messages.length === 0) {
        showEmpty();
    } else {
        // appendMessage's scroll ran while `log` was still detached (scrollHeight
        // 0), so jump to the latest message once this panel is mounted and laid
        // out. The timeout backstops the rAF in case rendered Markdown/math
        // changes the height a tick later.
        var scrollLatest = function () { log.scrollTop = log.scrollHeight; };
        if (window.requestAnimationFrame) {
            window.requestAnimationFrame(scrollLatest);
        }
        setTimeout(scrollLatest, 60);
    }

    //----------------------------------------------------

    // Picked-but-not-yet-sent attachments, GPT/Gemini-style: each is shown as a
    // chip (image thumbnail or a file glyph + name) in the strip above the bar
    // and can be removed individually. On send, they're posted as a multipart
    // form (see submit()); the server stores each file and references it on the
    // turn. The list is cleared once the message is sent.
    var attachments  = [];   // [{ id, file, name, size, isImage, url }]
    var nextAttachId = 1;
    var dragId       = null; // id of the chip currently being dragged to reorder

    // Human-readable byte count for the file-chip subtitle (1023 B, 4.2 KB, ...).
    function humanSize(bytes) {
        if (!bytes && bytes !== 0) { return ""; }
        var units = ["B", "KB", "MB", "GB"];
        var n = bytes, u = 0;
        while (n >= 1024 && u < units.length - 1) { n /= 1024; u ++; }
        return (u === 0 ? n : Math.round(n * 10) / 10) + " " + units[u];
    };

    // Hidden native picker; the paperclip button proxies clicks to it.
    //
    // `accept` must be non-empty: with none, some mobile browsers (Android WebViews,
    // WeChat/UC/QQ) treat a bare `type=file` as a capture input and open the camera
    // directly instead of the file chooser.
    //
    // On Android, though, a *narrow* list breaks the picker outright: Chrome turns
    // the MIME/extension list into an ACTION_GET_CONTENT intent, and on many devices
    // (esp. domestic ROMs) nothing is registered to satisfy the exotic Office/vendor
    // types, so tapping the paperclip just toasts "no app can perform this action"
    // and no file (not even an image) can be picked. `*/*` is still non-empty (so it
    // keeps the WebView camera workaround) but always resolves to the system file
    // manager. Other platforms keep the curated list so the chooser pre-filters; the
    // server accepts any bytes either way.
    var fileAccept = isAndroid()
        ? "*/*"
        : [
            "image/*",
            "application/pdf",
            "text/plain", "text/csv", "text/markdown", "text/xml",
            "application/json", "application/xml",
            "application/msword",
            "application/vnd.openxmlformats-officedocument.wordprocessingml.document",
            "application/vnd.ms-excel",
            "application/vnd.openxmlformats-officedocument.spreadsheetml.sheet",
            ".txt", ".csv", ".md", ".json", ".xml", ".pdf",
            ".doc", ".docx", ".xls", ".xlsx"
        ].join(",");
    var fileInput = ui.dom("input", { display: "none" }, {
        type: "file", multiple: "multiple",
        accept: fileAccept
    });

    // The strip of attachment chips above the input bar; collapsed when empty.
    var previews = ui.dom("div", {
        display      : "none",
        flexWrap     : "wrap",
        gap          : "8px",
        marginBottom : "10px",
        width        : "100%",
        maxWidth     : THREAD_MAX,
        marginInline : "auto"
    });

    // The small dark "x" that detaches one file; overlaid on the chip's
    // trailing-top corner (logical inset so it mirrors in RTL).
    function removeButton(id) {
        var b = ui.dom("button", {
            position       : "absolute",
            top            : "-6px",
            insetInlineEnd : "-6px",
            width          : "18px",
            height         : "18px",
            padding        : "0",
            borderRadius   : "50%",
            border         : "1px solid " + color.background,
            background     : color.onSurface,
            color          : color.onPrimary,
            cursor         : "pointer",
            display        : "flex",
            alignItems     : "center",
            justifyContent : "center",
            lineHeight     : "0"
        }, { type: "button", title: t("removeFile") });
        ui.setHTML(b, CLOSE_SVG);
        b.addEventListener("click", function () { removeAttachment(id); });
        return b;
    };

    function indexOfAttachment(id) {
        for (var i = 0; i < attachments.length; i ++) {
            if (attachments[i].id === id) { return i; }
        }
        return -1;
    };

    // Move the dragged chip to the dropped-on chip's slot. Splicing the source
    // out then back in at the target index lands it before the target when
    // dragging up and after it when dragging down -- i.e. where it visually drops.
    function reorderAttachment(fromId, toId) {
        var from = indexOfAttachment(fromId);
        var to   = indexOfAttachment(toId);
        if (from < 0 || to < 0 || from === to) { return; }
        var moved = attachments.splice(from, 1)[0];
        attachments.splice(to, 0, moved);
        renderPreviews();
    };

    // Make a chip a reorder drag source + drop target. id is passed in (not
    // closed over a loop variable) so each chip rebinds to its own attachment.
    // External file drops carry dataTransfer.files; reorder drags don't, and the
    // dragId guards below keep the bar's file-drop and this reorder from crossing.
    function makeDraggable(el, id) {
        el.setAttribute("draggable", "true");
        el.style.cursor = "grab";
        el.addEventListener("dragstart", function (evt) {
            dragId = id;
            el.style.opacity = "0.4";
            if (evt.dataTransfer) {
                evt.dataTransfer.effectAllowed = "move";
                // Firefox won't start a drag unless some data is set.
                try { evt.dataTransfer.setData("text/plain", String(id)); } catch (e) {}
            }
        });
        el.addEventListener("dragend", function () {
            dragId = null;
            el.style.opacity = "";
        });
        el.addEventListener("dragover", function (evt) {
            if (dragId === null || dragId === id) { return; }
            evt.preventDefault();   // mark this chip as a valid drop target
            if (evt.dataTransfer) { evt.dataTransfer.dropEffect = "move"; }
        });
        el.addEventListener("drop", function (evt) {
            if (dragId === null || dragId === id) { return; }
            evt.preventDefault();
            evt.stopPropagation();  // don't let the bar treat this as a file drop
            reorderAttachment(dragId, id);
        });
    };

    // Rebuild the preview strip from `attachments`. Images render as a square
    // thumbnail; everything else as a chip with a file glyph, name and size.
    // Each is drag-reorderable within the strip (see makeDraggable).
    function renderPreviews() {
        ui.clear(previews);
        if (attachments.length === 0) {
            previews.style.display = "none";
            return;
        }
        previews.style.display = "flex";

        for (var i = 0; i < attachments.length; i ++) {
            var att = attachments[i];
            var el;

            if (att.isImage) {
                el = ui.dom("div", {
                    position     : "relative",
                    width        : "56px",
                    height       : "56px",
                    flex         : "0 0 auto",
                    borderRadius : "12px",
                    overflow     : "visible"
                });
                var holder = ui.dom("div", {
                    width        : "56px",
                    height       : "56px",
                    borderRadius : "12px",
                    overflow     : "hidden",
                    border       : "1px solid " + color.outlineVar
                });
                holder.appendChild(ui.img(att.url, {
                    width         : "100%",
                    height        : "100%",
                    objectFit     : "cover",
                    display       : "block",
                    pointerEvents : "none"   // let the tile own the drag, not the img
                }));
                el.appendChild(holder);
            } else {
                el = ui.dom("div", {
                    position     : "relative",
                    display      : "flex",
                    alignItems   : "center",
                    gap          : "8px",
                    maxWidth     : "220px",
                    padding      : "8px 10px",
                    borderRadius : "12px",
                    border       : "1px solid " + color.outlineVar,
                    background   : color.background
                });
                el.appendChild(ui.setHTML(ui.dom("span", {
                    display : "flex",
                    flex    : "0 0 auto",
                    color   : color.onSurfaceVar
                }), FILE_SVG));

                var meta = ui.dom("div", { minWidth: "0", display: "flex", flexDirection: "column" });
                meta.appendChild(ui.setText(ui.dom("div", {
                    fontSize     : "0.85rem",
                    color        : color.onSurface,
                    whiteSpace   : "nowrap",
                    overflow     : "hidden",
                    textOverflow : "ellipsis"
                }), att.name));
                meta.appendChild(ui.setText(ui.dom("div", {
                    fontSize : "0.7rem",
                    color    : color.onSurfaceVar
                }), humanSize(att.size)));
                el.appendChild(meta);
            }

            makeDraggable(el, att.id);
            el.appendChild(removeButton(att.id));
            previews.appendChild(el);
        }
    };

    function addFiles(fileList) {
        for (var i = 0; i < fileList.length; i ++) {
            var f = fileList[i];
            var isImage = /^image\//.test(f.type);
            attachments.push({
                id      : nextAttachId ++,
                file    : f,
                name    : f.name,
                size    : f.size,
                isImage : isImage,
                // Object URLs back the thumbnails; revoked on remove/clear so
                // they don't leak.
                url     : isImage ? URL.createObjectURL(f) : ""
            });
        }
        renderPreviews();
    };

    function removeAttachment(id) {
        for (var i = 0; i < attachments.length; i ++) {
            if (attachments[i].id === id) {
                if (attachments[i].url) { URL.revokeObjectURL(attachments[i].url); }
                attachments.splice(i, 1);
                break;
            }
        }
        renderPreviews();
    };

    function clearAttachments() {
        for (var i = 0; i < attachments.length; i ++) {
            if (attachments[i].url) { URL.revokeObjectURL(attachments[i].url); }
        }
        attachments.length = 0;
        renderPreviews();
    };

    fileInput.addEventListener("change", function () {
        if (fileInput.files && fileInput.files.length) {
            addFiles(fileInput.files);
        }
        fileInput.value = "";   // let the same file be re-picked after removal
    });

    //----------------------------------------------------

    // Borderless now: the rounded `bar` below owns the outline and focus ring, so
    // the attach button, text area and Send button read as one composer.
    var input = ui.dom("textarea", {
        width        : "100%",
        resize       : "none",
        padding      : "4px 6px 2px",
        border       : "none",
        font         : "inherit",
        fontSize     : "1rem",
        lineHeight   : "1.5",
        color        : color.onSurface,
        background   : "transparent",
        outline      : "none",
        maxHeight    : "160px",
        boxSizing    : "border-box"
    }, { placeholder: t("askAnything"), rows: "1" });

    var attach = iconButton(ATTACH_SVG, t("attachFile"));
    attach.style.flex = "0 0 auto";
    // Match the 40px send button so that, with the bar's flex-end alignment, the
    // paperclip is vertically centered against a single-line composer (rather than
    // sitting ~4px low as a 32px button would).
    attach.style.width  = "40px";
    attach.style.height = "40px";
    attach.addEventListener("click", function () { fileInput.click(); });

    // Circular navy send button with an up-arrow (Theta Health style). While a
    // reply streams it flips to a STOP control -- spinner ring around a ■, same
    // interaction as the harmony client -- that interrupts the in-flight turn.
    var send = ui.dom("button", {
        flex: "0 0 auto", width: "40px", height: "40px", padding: "0",
        borderRadius: "50%", border: "none",
        background: color.primary, color: color.onPrimary,
        display: "flex", alignItems: "center", justifyContent: "center", cursor: "pointer"
    }, { type: "button", title: t("send") });
    ui.setHTML(send, SEND_ARROW_SVG);

    // Kill switch for the in-flight turn; submit() arms it, finish() clears it.
    var activeStop = null;
    send.addEventListener("click", function () {
        if (state.streaming && activeStop) { activeStop(); }
    });

    // busy: spinner + ■ acting as stop; idle: the up-arrow acting as send. The
    // busy button must not re-submit the form, so its type flips with it.
    function setSendBusy(busy) {
        send.setAttribute("type", busy ? "button" : "submit");
        send.title = busy ? t("stop") : t("send");
        if (!busy) {
            ui.setHTML(send, SEND_ARROW_SVG);
            return;
        }
        ui.clear(send);
        var wrap = ui.dom("span", {
            position: "relative", display: "inline-flex", width: "20px", height: "20px",
            alignItems: "center", justifyContent: "center"
        });
        // mb-spin keyframes live in index.css -- the CSP blocks injected <style>.
        wrap.appendChild(ui.dom("span", {
            position: "absolute", top: "0", left: "0", right: "0", bottom: "0",
            border: "2px solid transparent",
            borderTopColor: color.onPrimary, borderRadius: "50%",
            animation: "mb-spin 0.8s linear infinite"
        }));
        var square = ui.dom("span", { fontSize: "9px", lineHeight: "1" });
        ui.setText(square, "■");
        wrap.appendChild(square);
        send.appendChild(wrap);
    };

    // Subject selector — picking a care-circle member sends `subject` so the AI's
    // family_health tool defaults to that member ("how is Mom doing?"). Shown only
    // when members shared their health data. The "Currently for" label text was
    // dropped by request; the dropdown alone carries the subject (its "Me" option
    // is the default). Responsive placement: on a wide screen it sits inline in the
    // middle of the composer's control row (also acting as the flex spacer); on a
    // narrow phone it moves into the top bar's otherwise-empty center slot. The
    // breakpoint (isMobile) is re-read on each render, and index.js re-renders when
    // it's crossed, so the placement follows a resize/rotation.
    var isNarrow = isMobile();
    // Narrow puts this picker in the top bar (page background), wide leaves it in
    // the composer bar -- so its popup surface differs by placement.
    var _subject = caretSelect(isNarrow ? "44vw" : "320px",
                               isNarrow ? color.background : color.surfaceLow);
    var subjectSelect = _subject.select;
    subjectSelect.addEventListener("change", function () { state.currentSubjectId = subjectSelect.value; });
    var subjectInner = ui.dom("div", {
        display: "flex", alignItems: "center", gap: "6px", minWidth: "0"
    });
    subjectInner.appendChild(_subject.wrap);

    // Narrow: subjectInner goes into the top bar's center slot. Wide: an inline
    // centered slot that also acts as the control row's flex spacer, so it must
    // persist even when hidden (toggleSubject then flips only the inner content).
    // Either way subjectInner starts hidden until we know a subject exists.
    var subjectSlot = null;
    subjectInner.style.display = "none";
    if (isNarrow) {
        app.slots.topCenter = subjectInner;
    } else {
        subjectSlot = ui.dom("div", {
            flex: "1 1 0", minWidth: "0", display: "flex",
            alignItems: "center", justifyContent: "center"
        });
        subjectSlot.appendChild(subjectInner);
    }
    function toggleSubject(shown) {
        subjectInner.style.display = shown ? "flex" : "none";
        // Narrow only: the picker and the top bar's wordmark are alternatives for the
        // same row, so whichever is off duty gives way. This runs from an async
        // /api/circle callback, by which time the bar exists (app.js builds the body
        // first, the bar second, and both are synchronous) -- but the guard keeps it
        // honest if that order ever changes. Wide leaves the wordmark alone: the picker
        // sits in the composer there, not in the bar.
        if (isNarrow && app.slots.wordmark) {
            app.slots.wordmark.style.display = shown ? "none" : "block";
        }
    }
    function loadSubjects() {
        net.get("/api/circle/health-shared-with-me", function (d) {
            var users = (d && d.users instanceof Array) ? d.users : [];
            ui.clear(subjectSelect);
            var me = ui.dom("option", null, { value: "" }); ui.setText(me, t("chatSubjectMe"));
            subjectSelect.appendChild(me);
            var shown = 0;
            for (var i = 0; i < users.length; i ++) {
                var u = users[i];
                if (u.member == null) { continue; }   // no usable handle — skip (don't make value "undefined")
                var op = ui.dom("option", null, { value: String(u.member) });
                ui.setText(op, u.nickname || u.email || ("#" + u.member));
                subjectSelect.appendChild(op);
                shown ++;
            }
            subjectSelect.value = state.currentSubjectId || "";
            if (subjectSelect.value !== (state.currentSubjectId || "")) {   // stale selection
                state.currentSubjectId = ""; subjectSelect.value = "";
            }
            toggleSubject(shown > 0);
        }, function () { toggleSubject(false); });
    }
    loadSubjects();

    // Two-row composer (GPT/Claude style): the text area fills the top; the bottom
    // control row is + (attach) on the leading edge and Send on the trailing edge,
    // with the model picker between them. The rounded container owns the outline +
    // focus/drag ring so it reads as one field.
    var controls = ui.dom("div", {
        display: "flex", alignItems: "center", gap: "8px", width: "100%"
    });
    controls.appendChild(attach);
    if (isNarrow) {
        // Mobile: Send pinned to the trailing edge; the model picker centered in the
        // flex-grow slot between attach and Send (attach/Send are equal-width, so the
        // picker reads as horizontally centered in the bar).
        var modelSlot = ui.dom("div", {
            flex: "1 1 auto", minWidth: "0", display: "flex",
            alignItems: "center", justifyContent: "center"
        });
        modelSlot.appendChild(providerWrap);
        controls.appendChild(modelSlot);
        controls.appendChild(send);
    } else {
        // Wide: attach and Send are equal-width (40px) fixed ends; between them the
        // subject and model pickers split the bar into two equal halves (flex 1 1 0
        // each), each picker horizontally centered in its own half. The model gets
        // its own centered slot rather than sitting glued to Send.
        var modelSlot = ui.dom("div", {
            flex: "1 1 0", minWidth: "0", display: "flex",
            alignItems: "center", justifyContent: "center"
        });
        modelSlot.appendChild(providerWrap);
        controls.appendChild(subjectSlot);
        controls.appendChild(modelSlot);
        controls.appendChild(send);
    }

    var bar = ui.dom("div", {
        display       : "flex",
        flexDirection : "column",
        gap           : "6px",
        padding       : "8px 10px 8px 12px",
        border        : "1px solid " + color.outlineVar,
        borderRadius  : "20px",
        background    : color.surfaceLow,
        width         : "100%",
        maxWidth      : THREAD_MAX,
        marginInline  : "auto",
        boxSizing     : "border-box"
    });
    bar.appendChild(input);
    bar.appendChild(controls);

    input.addEventListener("focus", function () { bar.style.borderColor = color.primary; });
    input.addEventListener("blur",  function () { bar.style.borderColor = color.outlineVar; });

    // Drag-and-drop onto the composer, like GPT/Gemini: highlight on drag-over,
    // attach the dropped files on drop.
    bar.addEventListener("dragover", function (evt) {
        evt.preventDefault();
        bar.style.borderColor = color.primary;
    });
    bar.addEventListener("dragleave", function () {
        bar.style.borderColor = color.outlineVar;
    });
    bar.addEventListener("drop", function (evt) {
        evt.preventDefault();
        bar.style.borderColor = color.outlineVar;
        if (evt.dataTransfer && evt.dataTransfer.files && evt.dataTransfer.files.length) {
            addFiles(evt.dataTransfer.files);
        }
    });

    // No rule above the composer: the bar's own rounded outline already separates it
    // from the thread, and a second line just below the scroll edge read as clutter.
    var form = ui.dom("form", {
        flex          : "0 0 auto",
        display       : "flex",
        flexDirection : "column",
        padding       : "12px 16px 16px"
    });
    // The "AI can be wrong" caption, under the input like every other chat client.
    // It lives inside the form so a read-only shared conversation -- which drops the
    // whole composer -- drops this with it: there's nothing to double-check when you
    // can't ask anything. Same column as the bar above it, so it wraps in step.
    var disclaimer = ui.setText(ui.dom("div", {
        width        : "100%",
        maxWidth     : THREAD_MAX,
        marginInline : "auto",
        marginTop    : "8px",
        fontSize     : "0.7rem",
        lineHeight   : "1.35",
        color        : color.onSurfaceVar,
        textAlign    : "center"
    }), t("aiDisclaimer"));

    send.setAttribute("type", "submit");
    form.appendChild(previews);
    form.appendChild(bar);   // narrow "currently for" is in the top bar; wide is inline in the bar
    form.appendChild(disclaimer);
    form.appendChild(fileInput);

    //----------------------------------------------------

    function submit() {
        var text = input.value.trim();
        if (!text || state.streaming) {
            return;
        }

        if (state.messages.length === 0) {
            ui.clear(thread);
            // First turn clears the empty-state hero (incognito ghost included);
            // keep a slim banner so the session stays visibly private.
            if (state.incognito) { thread.appendChild(incognitoBanner()); }
        }

        var userTs = Date.now();
        state.messages.push({ role: "user", content: text, ts: userTs });
        // Incognito: keep the turn in memory (so the ephemeral thread is
        // multi-turn) but never mirror it to IndexedDB.
        if (!state.incognito) { db.saveMessages(state.messages); }
        // Snapshot the picked files in their arranged order for this turn, before
        // the chips are cleared; agent mode posts them as a multipart form below.
        var turnFiles = attachments.map(function (a) { return a.file; });

        appendMessage("user", text, userTs);
        input.value = "";
        input.style.height = "auto";
        clearAttachments();   // chips are client-only; reset once the turn is sent

        state.streaming = true;
        setSendBusy(true);

        var assistantTs = Date.now();
        var turnProvider = state.provider;   // provider used for this turn (label + persisted)
        var assistantCost = null;            // set from the costStatistics event (agent path)
        var bubble = appendMessage("assistant", "", assistantTs);
        // Hold the reply's place with the pulsing dots until its first token
        // lands. (This replaces a blink animation on the bubble itself, which
        // had nothing to show while the bubble was empty -- which is the whole
        // stretch it was meant to cover.)
        var dots = typingDots();
        bubble.appendChild(dots);
        var acc = "";

        // Drop the placeholder. Idempotent, and safe after a render has already
        // replaced the bubble's content: every exit from a turn runs it, since
        // one of them (an error with no text) leaves the bubble untouched.
        function stopDots() {
            if (dots && dots.parentNode) { dots.parentNode.removeChild(dots); }
            dots = null;
        };

        function finish(assistantText) {
            state.streaming = false;
            setSendBusy(false);
            activeStop = null;
            stopDots();
            input.focus();
            if (ui.isString(assistantText)) {
                renderInto(bubble, assistantText);   // final, un-throttled render
                // Persist cost + provider alongside the reply so a reload can
                // rebuild the footer (stats icon).
                state.messages.push({
                    role     : "assistant",
                    content  : assistantText,
                    ts       : assistantTs,
                    cost     : assistantCost,
                    provider : turnProvider
                });
                if (!state.incognito) { db.saveMessages(state.messages); }
            }
        };

        //----------------------------------------------------

        // Run the selection and stream its {type, content} events. {agent, provider}
        // come straight from the picked item (providerAgent map, built from the
        // grouped /api/providers response) -- no string parsing. providerName is the
        // model; agentName is "" for the default agent (the server resolves it).
        // (An empty selection is only possible when no models were on offer; handled
        // after this block.)
        if (state.provider) {
            var providerName = state.provider;
            var agentName    = providerAgent[state.provider] || "";

            // An agent error arrives as a {type:"error"} chunk via onmessage, but
            // the stream then still closes and fires oncomplete -- guard so we
            // finish exactly once.
            var handled = false;

            // Reasoning ("thinking") streams before the answer, in a collapsible
            // block above the reply (same interaction as harmony's RichMessage):
            // while the model is still thinking the thought stream shows expanded,
            // live; once the answer starts it folds to a one-line handle the user
            // can reopen. A manual toggle always wins over the automatic rule.
            var thinkAcc = "";
            var thinkBox = null;      // header + body container
            var thinkEmoji = null;    // 🤔 while thinking, 💭 once answered
            var thinkArrow = null;    // ▾ open / ▸ folded
            var thinkBody = null;     // the thought text
            var thinkToggled = false; // user clicked: manual state wins from then on
            var thinkManual = false;
            function thinkingLive() { return !handled && !acc; }
            function thinkingExpanded() { return thinkToggled ? thinkManual : thinkingLive(); }
            function syncThinking() {
                if (!thinkBox) { return; }
                ui.setText(thinkEmoji, thinkingLive() ? "🤔" : "💭");
                ui.setText(thinkArrow, thinkingExpanded() ? "▾" : "▸");
                thinkBody.style.display = thinkingExpanded() ? "" : "none";
            };
            function appendThinking(text) {
                thinkAcc += text;
                if (!thinkBox) {
                    var trow = ui.dom("div", { display: "flex", justifyContent: "flex-start" });
                    thinkBox = ui.dom("div", {
                        width        : "100%",
                        padding      : "6px 8px",
                        background   : color.overlay,
                        borderRadius : "8px"
                    });
                    thinkEmoji = ui.dom("span", { fontSize: "0.75rem" });
                    var label = ui.dom("span", {
                        fontSize : "0.75rem",
                        color    : color.onSurfaceVar
                    });
                    ui.setText(label, t("thinking"));
                    thinkArrow = ui.dom("span", {
                        fontSize : "0.75rem",
                        color    : color.onSurfaceVar
                    });
                    var head = ui.dom("div", {
                        display    : "flex",
                        alignItems : "center",
                        gap        : "6px",
                        cursor     : "pointer",
                        userSelect : "none"
                    });
                    head.appendChild(thinkEmoji);
                    head.appendChild(label);
                    head.appendChild(thinkArrow);
                    head.addEventListener("click", function () {
                        thinkManual = !thinkingExpanded();
                        thinkToggled = true;
                        syncThinking();
                    });
                    thinkBody = ui.dom("div", {
                        whiteSpace : "pre-wrap",
                        wordWrap   : "break-word",
                        fontSize   : "0.85rem",
                        lineHeight : "1.5",
                        fontStyle  : "italic",
                        marginTop  : "4px",
                        color      : color.onSurfaceVar
                    });
                    thinkBox.appendChild(head);
                    thinkBox.appendChild(thinkBody);
                    trow.appendChild(thinkBox);
                    thread.insertBefore(trow, bubble.parentNode); // above the reply row
                }
                ui.setText(thinkBody, thinkAcc);
                syncThinking();
                followBottom();
            };

            // Upload/extraction progress streams before the answer: the server
            // stores each attached file and extracts text from non-text uploads
            // up front, emitting upload / transcript events. Surface them in
            // a dim block above the reply, created lazily on the first event. The
            // per-file "reading…" line is dropped once that file's extraction
            // finishes (the upload line stays as a record).
            var statusBox = null;
            var readingLines = {};            // filename -> its "reading…" line
            var uploadingLines = {};          // filename -> its "uploading…" spinner line
            function statusBlock() {
                if (!statusBox) {
                    var srow = ui.dom("div", { display: "flex", justifyContent: "flex-start" });
                    statusBox = ui.dom("div", {
                        fontSize   : "0.8rem",
                        lineHeight : "1.5",
                        width      : "100%",
                        color      : color.onSurfaceVar
                    });
                    srow.appendChild(statusBox);
                    thread.insertBefore(srow, bubble.parentNode); // above the reply row
                }
                return statusBox;
            };
            // Per-file "uploading…" line with a spinner, shown the moment the
            // turn is sent (the bytes ride the request itself, so this is the
            // only feedback until the server's upload event lands). The
            // mb-spin keyframes live in index.css -- the CSP blocks injected
            // <style>, inline element styles may only reference them.
            function appendUploading(name) {
                if (!name || uploadingLines[name]) { return; }
                var line = ui.dom("div", { fontStyle: "italic" });
                line.appendChild(ui.dom("span", {
                    display         : "inline-block",
                    width           : "9px",
                    height          : "9px",
                    border          : "2px solid " + color.onSurfaceVar,
                    borderTopColor  : "transparent",
                    borderRadius    : "50%",
                    marginInlineEnd : "6px",
                    animation       : "mb-spin 0.8s linear infinite"
                }));
                var label = ui.dom("span", {});
                ui.setText(label, t("uploadingFile", name));
                line.appendChild(label);
                uploadingLines[name] = line;
                statusBlock().appendChild(line);
                followBottom();
            };
            // The stream ended without upload confirmations (error, early
            // close): drop any still-spinning lines rather than leave them
            // animating a transfer that is over.
            function settleUploads() {
                for (var name in uploadingLines) {
                    var line = uploadingLines[name];
                    if (line && line.parentNode) { line.parentNode.removeChild(line); }
                }
                uploadingLines = {};
            };
            function appendUpload(file) {
                var name = (file && file.filename) || "";
                if (!name) { return; }
                // Resolve this file's spinner line in place; fall back to a
                // fresh line for an upload the client didn't initiate a
                // spinner for (e.g. duplicate filenames sharing one line).
                var line = uploadingLines[name];
                if (line) {
                    delete uploadingLines[name];
                    line.style.fontStyle = "";
                    ui.setText(line, t("fileUploaded", name));   // drops the spinner span
                } else {
                    line = ui.dom("div", {});
                    ui.setText(line, t("fileUploaded", name));
                    statusBlock().appendChild(line);
                }
                followBottom();
            };
            function appendTranscript(info) {
                var name = info.filename || "";
                if (!name) { return; }
                if (info.phase === "begin") {
                    var line = ui.dom("div", { fontStyle: "italic" });
                    ui.setText(line, t("readingFile", name));
                    readingLines[name] = line;
                    statusBlock().appendChild(line);
                    followBottom();
                } else { // done: drop the spinner line, extracted or not
                    var prev = readingLines[name];
                    if (prev && prev.parentNode) { prev.parentNode.removeChild(prev); }
                    delete readingLines[name];
                }
            };

            // Answer visuals (ECharts charts, generated images) can arrive among or
            // after the reply tokens, and the reply bubble is re-rendered (innerHTML)
            // on every token -- so they can't live inside it. They go in their own
            // block appended just below the reply row, created lazily on the first one.
            var visualsBox = null;
            function visualsBlock() {
                if (!visualsBox) {
                    visualsBox = ui.dom("div", {
                        display: "flex", flexDirection: "column", gap: "8px",
                        width: "100%", marginTop: "8px"
                    });
                    var row = bubble.parentNode;
                    row.parentNode.insertBefore(visualsBox, row.nextSibling); // below the reply
                }
                return visualsBox;
            };
            function appendChart(option) {
                if (!option || typeof option !== "object" || !Object.keys(option).length) { return; }
                var holder = ui.dom("div", { width: "100%", height: isMobile() ? "240px" : "320px" });
                visualsBlock().appendChild(holder);
                followBottom();
                ensureECharts().then(function (echarts) {
                    if (!echarts) { return; }
                    try {
                        // Shared theme: the validated series palette plus the app's own
                        // ink/hairline colors as chart chrome; mbPrepare() strips any
                        // model-supplied colors so the theme actually applies (and adds
                        // a legend when several series must be told apart). Guarded --
                        // if chart-theme.js didn't load, fall back to stock ECharts.
                        // Both mode + chrome are read at render time, so a chart picks
                        // up the theme in effect when it arrives (older canvases keep
                        // theirs until a re-render drops them, like a reload does).
                        var theme = window.mbChartTheme ? window.mbChartTheme(config.isDarkTheme(), {
                            ink: color.onSurface, inkDim: color.onSurfaceVar,
                            axis: color.outline, grid: color.outlineVar,
                            surface: "transparent"
                        }) : null;
                        var chart = echarts.init(holder, theme, { renderer: "canvas" });
                        chart.setOption(window.mbPrepare ? window.mbPrepare(option) : option);
                        window.addEventListener("resize", function () { chart.resize(); });
                    } catch (e) { /* bad option: leave an empty holder rather than crash */ }
                    followBottom();
                }, function () { /* echarts failed to load: leave the holder empty */ });
            };
            function appendImage(url) {
                if (!url) { return; }
                var img = ui.dom("img",
                    { maxWidth: "100%", height: "auto", borderRadius: "8px" }, { src: url });
                img.onload = followBottom;
                visualsBlock().appendChild(img);
                followBottom();
            };

            // Tool invocations, one expandable <details> card per tool_id in
            // the status block: the summary line shows "Calling <tool>…" with
            // a spinner while it runs, then "Called <tool>"; expanding shows
            // the arguments and the result as pretty-printed JSON. title /
            // arguments may stream in pieces, so both accumulate; detail
            // completes the card.
            var toolCards = {};               // internal key -> card state
            var toolKeys  = {};               // provider tool_id -> current internal key
            var toolSeq   = 0;                // internal key counter
            function prettyJson(s) {
                try { return JSON.stringify(JSON.parse(s), null, 2); } catch (e) { return s; }
            };
            function jsonPre(text) {
                var pre = ui.dom("pre", {
                    margin     : "0",
                    whiteSpace : "pre-wrap",
                    wordBreak  : "break-all",
                    fontFamily : "ui-monospace, SFMono-Regular, Menlo, Consolas, monospace",
                    fontSize   : "0.75rem",
                    maxHeight  : "200px",
                    overflow   : "auto",
                    direction  : "ltr",       // JSON stays LTR in RTL locales
                    textAlign  : "left"
                });
                ui.setText(pre, prettyJson(text));
                return pre;
            };
            function toolCard(key) {
                var box = ui.dom("details", { margin: "2px 0" });
                var summary = ui.dom("summary", { cursor: "pointer" });
                var spin = ui.dom("span", {
                    display         : "inline-block",
                    width           : "9px",
                    height          : "9px",
                    border          : "2px solid " + color.onSurfaceVar,
                    borderTopColor  : "transparent",
                    borderRadius    : "50%",
                    marginInlineEnd : "6px",
                    animation       : "mb-spin 0.8s linear infinite"
                });
                var label = ui.dom("span", { fontStyle: "italic" });
                summary.appendChild(spin);
                summary.appendChild(label);
                var body = ui.dom("div", {
                    margin       : "4px 0 6px",
                    padding      : "6px 8px",
                    background   : color.surfaceLow,
                    borderRadius : "6px"
                });
                box.appendChild(summary);
                box.appendChild(body);
                statusBlock().appendChild(box);
                var card = { name: "", args: "", result: null,
                             spin: spin, label: label, body: body, done: false };
                toolCards[key] = card;
                return card;
            };
            function sectionLabel(text) {
                var el = ui.dom("div", { fontWeight: "600", margin: "2px 0" });
                ui.setText(el, text);
                return el;
            };
            // Rebuild the card body: Arguments always (re-rendered as chunks
            // stream in, so expanding a running call shows them live; partial
            // JSON just shows raw until it completes), Result once it exists.
            function renderToolBody(card) {
                ui.clear(card.body);
                card.body.appendChild(sectionLabel(t("toolArguments")));
                card.body.appendChild(jsonPre(card.args || "{}"));
                if (card.result !== null) {
                    card.body.appendChild(sectionLabel(t("toolResult")));
                    card.body.appendChild(jsonPre(card.result));
                }
            };
            function appendToolCall(info) {
                // Cards are keyed indirectly: some providers reuse or omit
                // tool ids across calls, and a title landing on a finished
                // card would append to its label ("Called list_filesread_file").
                // A title for a done (or unknown) id starts a fresh card.
                var pid  = info.toolId || "?";
                var key  = toolKeys[pid];
                var card = key ? toolCards[key] : null;
                if (!card || (info.phase === "title" && card.done)) {
                    key = "t" + (++toolSeq);
                    toolKeys[pid] = key;
                    card = toolCard(key);
                }
                if (info.phase === "title") {
                    card.name += info.content;
                    ui.setText(card.label, t("callingTool", card.name));
                    renderToolBody(card);
                } else if (info.phase === "arguments") {
                    card.args += info.content;
                    renderToolBody(card);
                } else { // detail: the result -- the call is over
                    card.done   = true;
                    card.result = info.content;
                    if (card.spin.parentNode) { card.spin.parentNode.removeChild(card.spin); }
                    card.label.style.fontStyle = "";
                    ui.setText(card.label, t("calledTool", card.name));
                    renderToolBody(card);
                }
                followBottom();
            };
            // The stream ended with a call still pending (error, early close):
            // stop its spinner and freeze the label at the tool name.
            function settleTools() {
                for (var id in toolCards) {
                    var card = toolCards[id];
                    if (card.done) { continue; }
                    if (card.spin.parentNode) { card.spin.parentNode.removeChild(card.spin); }
                    card.label.style.fontStyle = "";
                    ui.setText(card.label, card.name || "?");
                }
            };

            // Per-message footer (built once the reply completes): the provider
            // label, the stats ($) icon, and a copy icon. assistantCost is
            // captured from the costStatistics event during the stream and also
            // persisted (see finish) so the footer can be rebuilt on reload.
            var footerAdded = false;
            function addFooter() {
                if (footerAdded) { return; }
                footerAdded = true;
                appendFooter(turnProvider, acc, assistantCost);
            };

            // Only an opaque member handle (positive integer) is a valid subject;
            // anything else (including a stale "undefined") means "myself" => "".
            var subjectId = /^[0-9]+$/.test(state.currentSubjectId) ? state.currentSubjectId : "";

            // With attachments, post a multipart form so the files ride along
            // (the server saves them to object storage and references them on the
            // turn); otherwise the lighter JSON body. Same fields either way.
            var agentBody;
            if (turnFiles.length) {
                agentBody = new FormData();
                agentBody.append("agent", agentName);
                agentBody.append("provider", providerName);
                agentBody.append("question", text);
                agentBody.append("language", state.language || "");
                if (state.currentConversationId) { agentBody.append("conversation_id", state.currentConversationId); }
                if (subjectId) { agentBody.append("subject", subjectId); }
                if (state.incognito) { agentBody.append("incognito", "true"); }
                for (var fi = 0; fi < turnFiles.length; fi ++) {
                    agentBody.append("file", turnFiles[fi], turnFiles[fi].name);
                    appendUploading(turnFiles[fi].name);
                }
            } else {
                agentBody = { agent: agentName, provider: providerName, question: text,
                              language: state.language, conversation_id: state.currentConversationId || "",
                              subject: subjectId, incognito: state.incognito };
            }

            var streamOnMessage = function (chunk) { // onmessage
                if (handled) { return; }
                var ev = parseAgentChunk(chunk);
                if (ev.conversation) {
                    // The server's thread id for this turn; remember it so the
                    // next turn continues the same thread and Share targets it, and
                    // mirror it to IndexedDB so a reload continues the same thread
                    // too. Incognito ids are ephemeral, so never persist them.
                    if (ev.conversation.id) {
                        state.currentConversationId = ev.conversation.id;
                        if (!state.incognito) { db.saveConversationId(state.currentConversationId); }
                    }
                    return;
                }
                if (ui.isString(ev.reply)) {
                    var firstReply = !acc;
                    acc += ev.reply;
                    stopDots();                  // the answer takes the place back
                    streamRender(bubble, acc);
                    if (firstReply) { syncThinking(); } // answer started: fold the thinking block
                    followBottom();
                } else if (ui.isString(ev.thinking)) {
                    appendThinking(ev.thinking);
                } else if (ev.upload) {
                    appendUpload(ev.upload);
                } else if (ev.transcript) {
                    appendTranscript(ev.transcript);
                } else if (ev.tool) {
                    appendToolCall(ev.tool);
                } else if (ev.chart) {
                    appendChart(ev.chart);
                } else if (ev.image) {
                    appendImage(ev.image);
                } else if (ev.cost) {
                    assistantCost = ev.cost;
                    addFooter();   // cost is the last event; don't wait for stream close
                } else if (ev.error) {
                    handled = true;
                    settleUploads();
                    settleTools();
                    syncThinking();
                    onStreamError(bubble, "Error: " + ev.error, finish);
                }
            };
            var streamOnComplete = function () { // oncomplete
                if (handled) { return; }
                handled = true;
                settleUploads();
                settleTools();
                syncThinking();
                addFooter();
                finish(acc);
            };
            var streamOnError = function (reason) { // onerror
                if (handled) { return; }
                handled = true;
                settleUploads();
                settleTools();
                syncThinking();
                // A 401 on the stream bounces to login centrally (see net.js),
                // so this path only handles genuine stream/transport errors.
                onStreamError(bubble, acc || ("Error: " + (ui.isString(reason) ? reason : "request failed")), finish);
            };

            // Stop (the send button while streaming): kill the transport, then
            // finalize locally -- an aborted stream fires no callbacks on purpose
            // (net.stream swallows AbortError; the ondevice bridge unregisters the
            // turn), so this is a stopped turn's only completion path. A partial
            // answer is kept, like harmony's onStop; a turn stopped before any
            // reply drops its empty bubble but keeps the user message (unlike
            // onStreamError, where popping it enables a clean retry).
            var streamCancel = null;   // kills the transport; set when the stream starts
            function stopTurn() {
                if (handled) { return; }
                handled = true;
                if (streamCancel) { try { streamCancel(); } catch (e) { /* already gone */ } }
                settleUploads();
                settleTools();
                syncThinking();
                if (acc) {
                    addFooter();
                    finish(acc);       // keep what streamed before the stop
                } else {
                    var row = bubble.parentNode;
                    if (row && row.parentNode) { row.parentNode.removeChild(row); }
                    finish(null);      // nothing arrived: no empty assistant message
                }
            };
            activeStop = stopTurn;

            // On-device (desktop): drive the local engine instead of the server SSE.
            // It emits the same {type:"reply"} chunk strings, so the handlers above
            // are reused verbatim; completion/errors arrive on the same callbacks.
            if (isOnDeviceProvider(turnProvider)) {
                var history = [];
                for (var hi = 0; hi < state.messages.length; hi ++) {
                    var hm = state.messages[hi];
                    if (hm && hm.content) { history.push({ role: hm.role, content: hm.content }); }
                }
                var odModel = modelFromProvider(turnProvider);
                onDeviceBridge().isReady(odModel).then(function (ready) {
                    if (handled) { return; }   // stopped while isReady was in flight
                    if (!ready) {
                        handled = true;
                        onStreamError(bubble, "Error: on-device model not downloaded.", finish);
                        showOnDeviceManager();
                        return;
                    }
                    var gen = onDeviceBridge().generate(odModel, history, streamOnMessage, streamOnComplete, streamOnError);
                    streamCancel = function () { gen.cancel(); };
                });
                return;
            }

            var ctl = net.stream("/api/chat", agentBody, streamOnMessage, streamOnComplete, streamOnError);
            streamCancel = function () { ctl.abort(); };
            return;
        }

        // Reached only when no agent/provider is selected -- i.e. the selector had
        // no models to offer. Surface it and drop the unsent turn.
        onStreamError(bubble, "Error: " + t("selectModel"), finish);
    };

    function onStreamError(bubble, text, finish) {
        ui.setText(bubble, text);
        // Drop the unanswered user turn so a retry doesn't duplicate it.
        state.messages.pop();
        if (!state.incognito) { db.saveMessages(state.messages); }
        finish(null);
    };

    //----------------------------------------------------

    form.addEventListener("submit", function (evt) {
        evt.preventDefault();
        submit();
    });
    input.addEventListener("keydown", function (evt) {
        if (evt.key === "Enter" && !evt.shiftKey) {
            evt.preventDefault();
            submit();
        }
    });
    input.addEventListener("input", function () {
        input.style.height = "auto";
        input.style.height = Math.min(input.scrollHeight, 160) + "px";
    });

    wrap.appendChild(log);
    // A conversation shared *to* the user is read-only: omit the composer so it
    // can't be continued (only its owner can).
    if (!state.readOnly) { wrap.appendChild(form); }
    return wrap;
};

//----------------------------------------------------------------------------

// Parse one agent-mode SSE payload: a {type, content} event. Returns {reply}
// for streamed answer text, {error} for an error event, {upload} /
// {transcript} for the upload-preprocess progress the server streams before the
// answer, {} for everything else (tool steps, cost, the terminal "end").
// ECharts is a doc-root static asset (static/echarts.min.js ~1 MB), loaded lazily
// the first time a chart event arrives so users who never see a chart don't pay
// for it up front. Mirrors the qrcode/tanka-signer lazy loads (see tanka.js).
// chart-theme.js (the shared mirobody chart theme, another static/ copy) rides
// along best-effort: if it fails to load, charts degrade to stock ECharts
// instead of never appearing.
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
    echartsPromise = window.echarts
        ? Promise.resolve(window.echarts)
        : Promise.all([load("/echarts.min.js", true), load("/chart-theme.js", false)])
            .then(function () { return window.echarts; });
    return echartsPromise;
};

function parseAgentChunk(chunk) {
    var json = null;
    try {
        json = JSON.parse(chunk);
    } catch (err) {
        return {};
    }
    if (json.type === "reply") {
        return { reply: json.content || "" };
    }
    if (json.type === "thinking") {
        return { thinking: json.content || "" };
    }
    if (json.type === "costStatistics" && json.cost) {
        return { cost: json.cost };
    }
    // A file was offloaded to object storage: json.file is its {filename,
    // mime_type, url, file_key} reference (filename also in json.content).
    if (json.type === "upload") {
        return { upload: json.file || { filename: json.content || "" } };
    }
    // Text extraction from a non-text upload: phase "begin" (started) / "done"
    // (finished, with `extracted` telling whether any text resulted).
    if (json.type === "transcript") {
        return { transcript: {
            filename  : json.content || "",
            phase     : json.phase || "",
            extracted : !!json.extracted
        } };
    }
    // An ECharts option, embedded as a real nested object (server parsed it), ready
    // for echarts.setOption(). A parse failure on the server degrades to {}.
    if (json.type === "chart") {
        return { chart: json.chart || {} };
    }
    // A generated/served image; content is its URL.
    if (json.type === "image") {
        return { image: json.content || "" };
    }
    if (json.type === "error") {
        return { error: json.content || "error" };
    }
    // The durable conversation (thread) id for this turn. content carries it as a
    // precise decimal string (JS loses integer precision past 2^53).
    if (json.type === "conversation") {
        return { conversation: { id: ui.isString(json.content) ? json.content
                    : (json.conversation_id != null ? String(json.conversation_id) : "") } };
    }
    // Tool-call lifecycle: one queryTitle / queryArguments / queryDetail
    // triple per invocation, correlated by tool_id (title = tool name,
    // arguments = request JSON, detail = result JSON; title/arguments may
    // stream in pieces). Rendered as an expandable card in the status block.
    if (json.type === "queryTitle" || json.type === "queryArguments" ||
        json.type === "queryDetail") {
        return { tool: {
            phase   : json.type === "queryTitle"     ? "title"
                    : json.type === "queryArguments" ? "arguments" : "detail",
            toolId  : json.tool_id || "",
            content : ui.isString(json.content) ? json.content : ""
        } };
    }
    return {};
};

//----------------------------------------------------------------------------

exports.buildChat = buildChat;
exports.toggleIncognito = toggleIncognito;

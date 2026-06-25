
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
const SEND_ARROW_SVG = icons.SEND_ARROW_SVG;

const formatLocalTime = require("./format").formatLocalTime;
const showCostModal   = require("./modals").showCostModal;
const buildSettingsMenu = require("./topbar").buildSettingsMenu;

var t = i18n.t;

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

    // Reads like the app's ProviderMenu: a borderless text button (titleSmall:
    // 14px/500) with a trailing caret, sitting in the top bar. appearance:none
    // hides the native arrow so the CARET_SVG overlay shows instead.
    var providerSelect = ui.dom("select", {
        appearance       : "none",
        WebkitAppearance : "none",
        MozAppearance    : "none",
        paddingBlock       : "8px",
        paddingInlineStart : "12px",
        paddingInlineEnd   : "30px",   // room for the caret on the trailing edge
        border           : "none",
        borderRadius     : "10px",
        font             : "inherit",
        fontSize         : "0.875rem",
        fontWeight       : "500",
        color            : color.onSurface,
        background       : "transparent",
        outline          : "none",
        cursor           : "pointer",
        maxWidth         : "55vw",
        textAlignLast    : "center"
    });
    providerSelect.addEventListener("change", function () {
        state.provider = providerSelect.value;
        if (state.provider) {
            localStorage.setItem(PROVIDER_KEY, state.provider);
        } else {
            localStorage.removeItem(PROVIDER_KEY);
        }
    });

    function setProviderOptions(items) {
        ui.clear(providerSelect);

        var names = [];
        for (var i = 0; i < items.length; i ++) {
            if (!items[i] || !items[i].name) {
                continue;
            }
            names.push(items[i].name);
            var opt = ui.dom("option", null, { value: items[i].name });
            ui.setText(opt, items[i].name);
            providerSelect.appendChild(opt);
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

    // Wrap the select with the caret overlay and hand it to the top bar's center
    // slot; the settings/sign-out menu goes to the right slot.
    var providerWrap = ui.dom("div", {
        position: "relative", display: "inline-flex", alignItems: "center",
        maxWidth: "100%", borderRadius: "10px"
    });
    providerWrap.appendChild(providerSelect);
    var caret = ui.dom("span", {
        position: "absolute", insetInlineEnd: "8px", display: "flex",
        pointerEvents: "none", color: color.onSurfaceVar
    });
    ui.setHTML(caret, CARET_SVG);
    providerWrap.appendChild(caret);

    app.slots.topCenter = providerWrap;
    app.slots.topRight  = buildSettingsMenu();

    //----------------------------------------------------

    // Readable centered column: messages are capped to THREAD_MAX and centered so
    // they don't sprawl edge-to-edge on wide screens. The composer is aligned to
    // the same width below.
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
        flexDirection : "column",
        gap           : "14px"
    });
    log.appendChild(thread);

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
                padding      : "10px 14px",
                // Sharp "tail" at the top-inline-end corner (top-right in LTR,
                // top-left in RTL); the other three corners stay rounded.
                borderStartStartRadius : "16px",
                borderStartEndRadius   : "4px",
                borderEndEndRadius     : "16px",
                borderEndStartRadius   : "16px",
                fontSize     : "1rem",
                lineHeight   : "1.5",
                maxWidth     : isMobile() ? "85%" : "320px",
                background   : "#1A1A1A",
                color        : color.onPrimary
            }), text);
        } else {
            // Assistant turn: rendered Markdown + math (class "md", styled in
            // index.css). No pre-wrap -- the block elements own their spacing.
            bubble = ui.dom("div", {
                wordWrap   : "break-word",
                fontSize   : "1rem",
                lineHeight : "1.6",
                width      : "100%",
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
                marginTop      : "-8px"
            });
            stamp.appendChild(ui.setText(ui.dom("div", {
                fontSize : "0.7rem",
                color    : color.onSurfaceVar,
                opacity  : "0.7"
            }), formatLocalTime(ts)));
            thread.appendChild(stamp);
        }

        log.scrollTop = log.scrollHeight;
        return bubble;
    };

    function iconButton(svg, title) {
        var b = ui.dom("button", {
            border         : "none",
            background     : "transparent",
            padding        : "0",
            cursor         : "pointer",
            color          : "#757575",
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
            fontSize   : "14px",
            fontWeight : "400",
            color      : "#757575"
        }), providerLabel || "");

        var actions = ui.dom("div", {
            flex           : "1 1 auto",
            display        : "flex",
            alignItems     : "center",
            justifyContent : "flex-end",
            gap            : "2px"
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
            justifyContent : "space-between",
            marginTop      : "4px"
        });
        row.appendChild(label);
        row.appendChild(actions);

        thread.appendChild(row);
        log.scrollTop = log.scrollHeight;
        return row;
    };

    function showEmpty() {
        var box = ui.dom("div", {
            margin    : "auto",
            textAlign : "center",
            maxWidth  : "640px",
            width     : "100%",
            display       : "flex",
            flexDirection : "column",
            alignItems    : "center",
            gap           : "22px",
            padding       : "24px"
        });
        // Serif heading (Theta Health style).
        box.appendChild(ui.setText(ui.dom("h2", {
            fontFamily : serifFamily,
            fontWeight : "600",
            fontSize   : "clamp(1.6rem, 4vw, 2.1rem)",
            color      : color.onSurface,
            margin     : "0"
        }), t("chatStart")));

        thread.appendChild(box);
    };

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
    var fileInput = ui.dom("input", { display: "none" }, {
        type: "file", multiple: "multiple"
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
        flex         : "1 1 auto",
        resize       : "none",
        padding      : "8px 4px",
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
    attach.addEventListener("click", function () { fileInput.click(); });

    // Circular navy send button with an up-arrow (Theta Health style); toggled
    // disabled while a reply streams (see submit()).
    var send = ui.dom("button", {
        flex: "0 0 auto", width: "40px", height: "40px", padding: "0",
        borderRadius: "50%", border: "none",
        background: color.primary, color: color.onPrimary,
        display: "flex", alignItems: "center", justifyContent: "center", cursor: "pointer"
    }, { type: "button", title: t("send") });
    ui.setHTML(send, SEND_ARROW_SVG);

    // The rounded composer bar: attach (paperclip) | text area | Send. Its border
    // tracks focus and drag-over (outlineVariant -> primary), as the field does.
    var bar = ui.dom("div", {
        display      : "flex",
        alignItems   : "flex-end",
        gap          : "8px",
        padding      : "6px 8px 6px 6px",
        border       : "1px solid " + color.outlineVar,
        borderRadius : "24px",
        background   : color.surfaceLow,
        width        : "100%",
        maxWidth     : THREAD_MAX,
        marginInline : "auto",
        boxSizing    : "border-box"
    });
    bar.appendChild(attach);
    bar.appendChild(input);
    bar.appendChild(send);

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

    var form = ui.dom("form", {
        flex          : "0 0 auto",
        display       : "flex",
        flexDirection : "column",
        padding       : "12px 16px 16px",
        borderTop     : "1px solid " + color.outlineVar
    });
    send.setAttribute("type", "submit");
    form.appendChild(previews);
    form.appendChild(bar);
    form.appendChild(fileInput);

    //----------------------------------------------------

    function submit() {
        var text = input.value.trim();
        if (!text || state.streaming) {
            return;
        }

        if (state.messages.length === 0) {
            ui.clear(thread);
        }

        var userTs = Date.now();
        state.messages.push({ role: "user", content: text, ts: userTs });
        db.saveMessages(state.messages);
        // Snapshot the picked files in their arranged order for this turn, before
        // the chips are cleared; agent mode posts them as a multipart form below.
        var turnFiles = attachments.map(function (a) { return a.file; });

        appendMessage("user", text, userTs);
        input.value = "";
        input.style.height = "auto";
        clearAttachments();   // chips are client-only; reset once the turn is sent

        state.streaming = true;
        send.disabled = true;

        var assistantTs = Date.now();
        var turnProvider = state.provider;   // provider used for this turn (label + persisted)
        var assistantCost = null;            // set from the costStatistics event (agent path)
        var bubble = appendMessage("assistant", "", assistantTs);
        bubble.style.animation = "mb-blink 1s steps(2, start) infinite";
        var acc = "";

        function finish(assistantText) {
            state.streaming = false;
            send.disabled = false;
            bubble.style.animation = "";
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
                db.saveMessages(state.messages);
            }
        };

        //----------------------------------------------------

        // Run the selected agent ("Agent/provider") and stream its {type, content}
        // events. A straight one-shot SSE stream. (An empty selection is only
        // possible when no models were on offer; handled after this block.)
        if (state.provider) {
            var slash = state.provider.indexOf("/");
            var agentName = slash >= 0 ? state.provider.slice(0, slash) : state.provider;
            var providerName = slash >= 0 ? state.provider.slice(slash + 1) : "";

            // An agent error arrives as a {type:"error"} chunk via onmessage, but
            // the stream then still closes and fires oncomplete -- guard so we
            // finish exactly once.
            var handled = false;

            // Reasoning ("thinking") streams before the answer. Render it in a
            // dim block above the reply, created lazily on the first token.
            var thinkAcc = "";
            var thinkBubble = null;
            function appendThinking(text) {
                thinkAcc += text;
                if (!thinkBubble) {
                    var trow = ui.dom("div", { display: "flex", justifyContent: "flex-start" });
                    thinkBubble = ui.dom("div", {
                        whiteSpace : "pre-wrap",
                        wordWrap   : "break-word",
                        fontSize   : "0.85rem",
                        lineHeight : "1.5",
                        fontStyle  : "italic",
                        width      : "100%",
                        color      : color.onSurfaceVar
                    });
                    trow.appendChild(thinkBubble);
                    thread.insertBefore(trow, bubble.parentNode); // above the reply row
                }
                ui.setText(thinkBubble, thinkAcc);
                log.scrollTop = log.scrollHeight;
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
                log.scrollTop = log.scrollHeight;
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
                log.scrollTop = log.scrollHeight;
            };
            function appendTranscript(info) {
                var name = info.filename || "";
                if (!name) { return; }
                if (info.phase === "begin") {
                    var line = ui.dom("div", { fontStyle: "italic" });
                    ui.setText(line, t("readingFile", name));
                    readingLines[name] = line;
                    statusBlock().appendChild(line);
                    log.scrollTop = log.scrollHeight;
                } else { // done: drop the spinner line, extracted or not
                    var prev = readingLines[name];
                    if (prev && prev.parentNode) { prev.parentNode.removeChild(prev); }
                    delete readingLines[name];
                }
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
                log.scrollTop = log.scrollHeight;
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
                for (var fi = 0; fi < turnFiles.length; fi ++) {
                    agentBody.append("file", turnFiles[fi], turnFiles[fi].name);
                    appendUploading(turnFiles[fi].name);
                }
            } else {
                agentBody = { agent: agentName, provider: providerName, question: text, language: state.language };
            }

            net.stream(
                "/api/chat",
                agentBody,
                function (chunk) { // onmessage
                    if (handled) { return; }
                    var ev = parseAgentChunk(chunk);
                    if (ui.isString(ev.reply)) {
                        acc += ev.reply;
                        streamRender(bubble, acc);
                        bubble.style.animation = ""; // stop the cursor blink once text streams
                        log.scrollTop = log.scrollHeight;
                    } else if (ui.isString(ev.thinking)) {
                        appendThinking(ev.thinking);
                    } else if (ev.upload) {
                        appendUpload(ev.upload);
                    } else if (ev.transcript) {
                        appendTranscript(ev.transcript);
                    } else if (ev.tool) {
                        appendToolCall(ev.tool);
                    } else if (ev.cost) {
                        assistantCost = ev.cost;
                        addFooter();   // cost is the last event; don't wait for stream close
                    } else if (ev.error) {
                        handled = true;
                        settleUploads();
                        settleTools();
                        onStreamError(bubble, "Error: " + ev.error, finish);
                    }
                },
                function () { // oncomplete
                    if (handled) { return; }
                    handled = true;
                    settleUploads();
                    settleTools();
                    addFooter();
                    finish(acc);
                },
                function (reason) { // onerror
                    if (handled) { return; }
                    handled = true;
                    settleUploads();
                    settleTools();
                    // A 401 on the stream bounces to login centrally (see net.js),
                    // so this path only handles genuine stream/transport errors.
                    onStreamError(bubble, acc || ("Error: " + (ui.isString(reason) ? reason : "request failed")), finish);
                }
            );
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
        db.saveMessages(state.messages);
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
    wrap.appendChild(form);
    return wrap;
};

//----------------------------------------------------------------------------

// Parse one agent-mode SSE payload: a {type, content} event. Returns {reply}
// for streamed answer text, {error} for an error event, {upload} /
// {transcript} for the upload-preprocess progress the server streams before the
// answer, {} for everything else (tool steps, cost, the terminal "end").
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
    if (json.type === "error") {
        return { error: json.content || "error" };
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

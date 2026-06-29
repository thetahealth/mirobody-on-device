
//----------------------------------------------------------------------------
// History drawer: a left-side panel listing past sessions (GET /api/history),
// each deletable (POST /api/history/delete). Mirrors the app's HistoryScreen --
// list + delete only, no resume. Opened from the brand logo.
//----------------------------------------------------------------------------

const ui   = require("./ui");
const net  = require("./net");
const i18n = require("./i18n");

const config = require("./config");
const color  = config.color;
const state  = config.state;

const button = require("./widgets").button;
const icons  = require("./icons");
const BACK_SVG  = icons.BACK_SVG;
const TRASH_SVG = icons.TRASH_SVG;

const formatTimestamp  = require("./format").formatTimestamp;
const showConfirmModal = require("./modals").showConfirmModal;

var t = i18n.t;

// User-adjustable drawer width, dragged via the edge handle and remembered
// across opens. Width is stored in px; the default mirrors the old
// min(85vw, 360px), and every applied value is clamped to [MIN, 90vw].
var WIDTH_KEY  = "mb-history-width";
var MIN_WIDTH  = 240;

//----------------------------------------------------------------------------

// Loads on open and shows loading / error+retry / empty / list states.
function openHistory() {
    var items = [];
    var rtl = document.documentElement.dir === "rtl";

    function maxWidth()    { return Math.round(window.innerWidth * 0.9); }
    function clampWidth(w) { return Math.max(MIN_WIDTH, Math.min(w, maxWidth())); }

    var storedWidth  = parseInt(localStorage.getItem(WIDTH_KEY), 10);
    var initialWidth = clampWidth(storedWidth > 0
        ? storedWidth
        : Math.min(360, Math.round(window.innerWidth * 0.85)));

    // justifyContent flex-start is direction-aware: the drawer slides in from the
    // inline-start edge (left in LTR, right in RTL).
    var backdrop = ui.dom("div", {
        position: "fixed", inset: "0", background: "rgba(0, 0, 0, 0.4)",
        display: "flex", justifyContent: "flex-start", zIndex: "1100"
    });

    var panel = ui.dom("aside", {
        position: "relative", width: initialWidth + "px", height: "100%",
        background: color.background,
        // Shadow falls away from the drawer's anchored edge.
        boxShadow: (rtl ? "-2px" : "2px") + " 0 24px rgba(0, 0, 0, 0.18)",
        display: "flex", flexDirection: "column", minHeight: "0"
    });

    function dismiss() {
        if (backdrop.parentNode) { backdrop.parentNode.removeChild(backdrop); }
    };

    // Header: back button + title.
    var header = ui.dom("div", {
        flex: "0 0 auto", display: "flex", alignItems: "center", gap: "4px",
        height: "56px", padding: "0 8px"
    });
    var backBtn = ui.dom("button", {
        border: "none", background: "transparent", cursor: "pointer",
        width: "40px", height: "40px", borderRadius: "20px",
        display: "flex", alignItems: "center", justifyContent: "center",
        color: color.onSurfaceVar,
        // The arrow is directional: mirror it so it points the right way in RTL.
        transform: rtl ? "scaleX(-1)" : "none"
    }, { type: "button", title: t("back") });
    ui.setHTML(backBtn, BACK_SVG);
    backBtn.addEventListener("click", dismiss);
    header.appendChild(backBtn);
    header.appendChild(ui.setText(ui.dom("div", {
        fontSize: "1rem", fontWeight: "500", color: color.onSurface
    }), t("historyTitle")));

    var body = ui.dom("div", { flex: "1 1 auto", overflowY: "auto", minHeight: "0" });

    // Drag handle straddling the drawer's inline-end edge (right in LTR, left in
    // RTL). A thin grip line highlights on hover/drag; pointer capture keeps the
    // drag alive even when the cursor outruns the 10px strip.
    var dragging = false;
    var grip = ui.dom("div", {
        width: "2px", height: "100%", background: "transparent",
        transition: "background 120ms"
    });
    var resizer = ui.dom("aside", {
        position: "absolute", top: "0", bottom: "0", insetInlineEnd: "0",
        width: "10px", transform: "translateX(" + (rtl ? "-50%" : "50%") + ")",
        zIndex: "2", cursor: "ew-resize", touchAction: "none",
        display: "flex", justifyContent: "center"
    }, { title: t("resize") });
    resizer.appendChild(grip);

    function highlight(on) { grip.style.background = on ? color.primary : "transparent"; }
    resizer.addEventListener("mouseenter", function () { highlight(true); });
    resizer.addEventListener("mouseleave", function () { if (!dragging) highlight(false); });

    var startX = 0, startWidth = 0;
    resizer.addEventListener("pointerdown", function (evt) {
        evt.preventDefault();
        dragging = true;
        startX = evt.clientX;
        startWidth = panel.getBoundingClientRect().width;
        highlight(true);
        document.body.style.userSelect = "none";
        resizer.setPointerCapture(evt.pointerId);
    });
    resizer.addEventListener("pointermove", function (evt) {
        if (!dragging) { return; }
        // Anchored at the inline-end edge: in RTL dragging toward the start (left)
        // grows the drawer, so the delta's sign flips.
        var delta = evt.clientX - startX;
        panel.style.width = clampWidth(startWidth + (rtl ? -delta : delta)) + "px";
    });
    function endDrag(evt) {
        if (!dragging) { return; }
        dragging = false;
        highlight(false);
        document.body.style.userSelect = "";
        try { resizer.releasePointerCapture(evt.pointerId); } catch (e) {}
        localStorage.setItem(WIDTH_KEY, String(Math.round(panel.getBoundingClientRect().width)));
    };
    resizer.addEventListener("pointerup", endDrag);
    resizer.addEventListener("pointercancel", endDrag);

    panel.appendChild(header);
    panel.appendChild(body);
    panel.appendChild(resizer);
    backdrop.appendChild(panel);
    backdrop.addEventListener("click", function (evt) {
        if (evt.target === backdrop) { dismiss(); }
    });
    document.body.appendChild(backdrop);

    //----------------------------------------------------

    function centerBox(node) {
        var box = ui.dom("div", {
            height: "100%", display: "flex", alignItems: "center",
            justifyContent: "center", padding: "24px", textAlign: "center"
        });
        box.appendChild(node);
        return box;
    };

    function showLoading() {
        ui.clear(body);
        // A lightweight spinner: reuse the streaming-cursor blink on a dot.
        var dot = ui.dom("div", {
            width: "10px", height: "10px", borderRadius: "50%",
            background: color.primary, animation: "mb-blink 1s steps(2, start) infinite"
        });
        body.appendChild(centerBox(dot));
    };

    function showError(msg) {
        ui.clear(body);
        var col = ui.dom("div", {
            display: "flex", flexDirection: "column", alignItems: "center", gap: "10px"
        });
        col.appendChild(ui.setText(ui.dom("div", {
            fontSize: "0.875rem", color: color.error
        }), msg || t("historyLoadFailed")));
        var retry = button(t("retry"), false, { click: load });
        ui.setStyle(retry, { padding: "8px 16px" });
        col.appendChild(retry);
        body.appendChild(centerBox(col));
    };

    function showEmpty() {
        ui.clear(body);
        body.appendChild(centerBox(ui.setText(ui.dom("div", {
            fontSize: "0.875rem", color: color.onSurfaceVar
        }), t("historyEmpty"))));
    };

    // Open a conversation into the chat view. Owned threads stay editable; one
    // shared *to* the user opens read-only (the composer is hidden). Best-effort:
    // a load failure leaves the drawer open.
    function openConversation(item) {
        net.get(
            "/api/conversation?id=" + encodeURIComponent(item.session_id),
            function (data) {
                var msgs = (data && data.messages instanceof Array) ? data.messages : [];
                state.messages = msgs.map(function (m) {
                    var o = { role: m.role, content: m.content || "", ts: m.created_at };
                    if (m.role === "assistant" && m.provider) { o.provider = m.provider; }
                    return o;
                });
                state.currentConversationId = String((data && data.id) || item.session_id);
                state.readOnly = !(data && data.owned);
                dismiss();
                require("./app").render();
            },
            function () {}
        );
    };

    function badge(text, strong) {
        return ui.setText(ui.dom("div", {
            display: "inline-block", fontSize: "0.68rem", padding: "1px 7px",
            borderRadius: "9px", whiteSpace: "nowrap", alignSelf: "flex-start",
            background: strong ? color.userBubble : color.surfaceLow,
            color: strong ? color.primary : color.onSurfaceVar
        }), text);
    };

    function row(item) {
        var wrap = ui.dom("div", {
            display: "flex", alignItems: "center",
            paddingBlock: "12px", paddingInlineStart: "20px", paddingInlineEnd: "8px",
            borderBottom: "1px solid " + color.outlineVar
        });
        wrap.style.borderBottomColor = "rgba(196, 199, 203, 0.4)";

        var texts = ui.dom("div", {
            flex: "1 1 auto", minWidth: "0", cursor: "pointer",
            display: "flex", flexDirection: "column", gap: "4px"
        });
        texts.addEventListener("click", function () { openConversation(item); });
        var title = item.summary || item.session_id || t("historyUntitled");
        texts.appendChild(ui.setText(ui.dom("div", {
            fontSize: "1rem", color: color.onSurface, lineHeight: "1.35",
            overflow: "hidden", textOverflow: "ellipsis",
            display: "-webkit-box", WebkitLineClamp: "2", WebkitBoxOrient: "vertical"
        }), title));
        // Sharing badges: "Shared by <owner>" on a thread shared to me; "Shared
        // with N" on one of my own that I've shared.
        if (item.owned === false && item.shared_by) {
            texts.appendChild(badge(t("sharedByBadge", item.shared_by), true));
        } else if (item.owned && item.shared_with_count > 0) {
            texts.appendChild(badge(t("sharedWithBadge", String(item.shared_with_count)), false));
        }
        if (item.timestamp) {
            texts.appendChild(ui.setText(ui.dom("div", {
                fontSize: "0.7rem", color: color.onSurfaceVar, opacity: "0.7"
            }), formatTimestamp(item.timestamp)));
        }

        var del = ui.dom("button", {
            border: "none", background: "transparent", cursor: "pointer",
            width: "36px", height: "36px", borderRadius: "18px", flex: "0 0 auto",
            display: "flex", alignItems: "center", justifyContent: "center",
            color: color.error
        }, { type: "button", title: t("delete") });
        ui.setHTML(del, TRASH_SVG);
        del.addEventListener("click", function () {
            showConfirmModal(
                t("historyDeleteTitle"), t("historyDeleteMessage"),
                t("delete"), true,
                function () { remove(item.session_id); }
            );
        });

        wrap.appendChild(texts);
        wrap.appendChild(del);
        return wrap;
    };

    function showList() {
        ui.clear(body);
        if (items.length === 0) { showEmpty(); return; }
        var list = ui.dom("div", { padding: "8px 0" });
        for (var i = 0; i < items.length; i ++) {
            list.appendChild(row(items[i]));
        }
        body.appendChild(list);
    };

    //----------------------------------------------------

    function load() {
        showLoading();
        net.get(
            "/api/history?page=0&page_size=20",
            function (data) { // onsuccess
                items = (data && data.summaries instanceof Array) ? data.summaries : [];
                showList();
            },
            function (msg, code) { // onerror (a 401 bounces to login centrally; see net.js)
                showError(ui.isString(msg) ? msg : t("historyLoadFailed"));
            }
        );
    };

    function remove(sessionId) {
        if (!sessionId) { return; }
        // Optimistically drop the row; restore by reloading if the call fails.
        var previous = items;
        items = items.filter(function (it) { return it.session_id !== sessionId; });
        showList();
        net.post(
            "/api/history/delete",
            { session_id: sessionId },
            null,
            function () { items = previous; showList(); }
        );
    };

    load();
};

//----------------------------------------------------------------------------

exports.openHistory = openHistory;

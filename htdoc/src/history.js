
//----------------------------------------------------------------------------
// Left navigation drawer -- the app's only menu, opened from the top-bar hamburger
// on every screen. Top: a "New chat" / "Incognito" button row. Middle (the only
// scrolling area): past sessions (GET /api/history, each deletable via POST
// /api/history/delete; tap to resume). Bottom (all PINNED): the Health & data group
// (care circle / EHR / devices), the app-settings group (language / font size /
// appearance / backend), and the account row (switch account / Sign out).
//
// There is no top-right settings gear any more: it held only the app settings, and
// was the sole reason the login screen had a right-hand action at all. Folding it in
// here leaves one menu affordance instead of two, and lets the login screen open
// this same drawer showing just the settings group -- everything else is
// session-scoped and appears once signed in.
//----------------------------------------------------------------------------

const ui   = require("./ui");
const net  = require("./net");
const i18n = require("./i18n");

const config = require("./config");
const color  = config.color;
const state  = config.state;
// The app-settings rows below (moved here from the old top-bar gear) need the same
// persistence keys, value labels and live-apply helpers the gear used.
const LANGUAGE_KEY   = config.LANGUAGE_KEY;
const FONT_KEY       = config.FONT_KEY;
const THEME_KEY      = config.THEME_KEY;
const languageLabel  = config.languageLabel;
const fontTierLabel  = config.fontTierLabel;
const themeLabel     = config.themeLabel;
const applyDirection = config.applyDirection;
const applyFontScale = config.applyFontScale;

const button = require("./widgets").button;
const icons  = require("./icons");
const BACK_SVG  = icons.BACK_SVG;
const TRASH_SVG = icons.TRASH_SVG;

const formatTimestamp  = require("./format").formatTimestamp;
const modals           = require("./modals");
const showConfirmModal = modals.showConfirmModal;

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
    // Signed out (the login screen's drawer) only the app-settings group applies:
    // history, New chat / Incognito, health connections and the account row are all
    // session-scoped and are skipped entirely.
    var signedIn = !!net.getToken();

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

    // Header: back button + title. (No build version here, and no About row below it
    // either -- the version isn't surfaced in the web client's UI at all.)
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
    }), t("menuTitle")));

    // History rows live in their own box so the loading / empty / error states clear
    // just the list, leaving the nav sections appended below it intact. The body as a
    // whole is only mounted for a signed-in session (see the panel assembly below).
    var body = ui.dom("div", { flex: "1 1 auto", overflowY: "auto", minHeight: "0" });
    var historyBox = ui.dom("div", { minHeight: "160px" });
    body.appendChild(historyBox);

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

    // --- Nav rows shared by the Health & data and Settings groups -------------
    function navRow(label, hint, onClick, danger) {
        var r = ui.dom("button", {
            display: "flex", alignItems: "center", justifyContent: "space-between",
            gap: "12px", textAlign: "start", width: "100%", border: "none",
            // minHeight matches the history rows' content (their 40px delete button);
            // with the same 7px block padding, both groups come out the same height.
            background: "transparent", padding: "7px 20px", minHeight: "40px",
            cursor: "pointer", font: "inherit", fontSize: "0.95rem",
            color: danger ? color.error : color.onSurface
        }, { type: "button" });
        r.appendChild(ui.setText(ui.dom("span", {
            whiteSpace: "nowrap", flex: "0 0 auto"
        }), label));
        if (hint) {
            // The hint takes the remaining width and ellipsizes -- the backend
            // address can be a long URL.
            r.appendChild(ui.setText(ui.dom("span", {
                fontSize: "0.8rem", color: color.onSurfaceVar, whiteSpace: "nowrap",
                overflow: "hidden", textOverflow: "ellipsis", minWidth: "0",
                flex: "0 1 auto", textAlign: "end"
            }), hint));
        }
        r.addEventListener("mouseenter", function () { r.style.background = color.surfaceLow; });
        r.addEventListener("mouseleave", function () { r.style.background = "transparent"; });
        r.addEventListener("click", onClick);
        return r;
    };
    function drawerDivider() {
        return ui.dom("div", {
            height: "1px", background: color.outlineVar, opacity: "0.5", margin: "4px 0"
        });
    };

    // The "New chat" / Incognito row only exists for a signed-in session; the login
    // screen's drawer has no thread to start or hide.
    var topRow = null;
    if (signedIn) {

        // "New chat": drop the current thread for a fresh one. Skipped mid-stream so an
        // in-flight reply isn't torn down. Leaves incognito state as-is (that's the
        // top-bar toggle's job); clears the local mirror so a reload doesn't resurrect
        // the old thread.
        var newChatBtn = ui.dom("button", {
            display: "flex", alignItems: "center", justifyContent: "center", gap: "10px",
            border: "1px solid " + color.outlineVar, borderRadius: "8px",
            background: "transparent", cursor: "pointer",
            font: "inherit", fontSize: "0.95rem", fontWeight: "500",
            color: color.onSurface, padding: "9px 14px", flex: "1 1 0", minWidth: "0"
        }, { type: "button" });
        newChatBtn.appendChild(ui.setText(ui.dom("span", { whiteSpace: "nowrap" }), t("newChat")));
        newChatBtn.addEventListener("mouseenter", function () { newChatBtn.style.background = color.surfaceLow; });
        newChatBtn.addEventListener("mouseleave", function () { newChatBtn.style.background = "transparent"; });
        newChatBtn.addEventListener("click", function () {
            if (state.streaming) { return; }
            state.messages = [];
            state.currentConversationId = "";
            state.readOnly = false;
            if (!state.incognito) {
                var freshDb = require("./db");
                freshDb.saveMessages([]);
                freshDb.saveConversationId("");   // forget the old thread id too
            }
            dismiss();
            require("./app").render();
        });

        // Incognito ("privacy mode") toggle, sharing the top row with "New chat" (New
        // chat on the leading edge, this on the trailing edge). A sibling text button:
        // ghost icon + an explicit label that states what the tap does ("Turn on /
        // off incognito"). Solid ghost + navy tint when on, hollow ghost when off;
        // toggling swaps the whole session (chat.toggleIncognito) then closes the
        // drawer so the re-rendered chat shows the incognito banner/empty state.
        var incognitoBtn = ui.dom("button", {
            display: "flex", alignItems: "center", justifyContent: "center", gap: "10px",
            border: "1px solid " + (state.incognito ? color.primary : color.outlineVar),
            borderRadius: "8px", background: "transparent", cursor: "pointer",
            font: "inherit", fontSize: "0.95rem", fontWeight: "500",
            color: state.incognito ? color.primary : color.onSurface,
            padding: "9px 14px", flex: "1 1 0", minWidth: "0"
        }, { type: "button", title: t(state.incognito ? "incognitoStop" : "incognitoStart") });
        incognitoBtn.setAttribute("aria-pressed", state.incognito ? "true" : "false");
        incognitoBtn.appendChild(ui.setText(ui.dom("span", { whiteSpace: "nowrap" }),
            t("incognitoMode")));
        incognitoBtn.addEventListener("mouseenter", function () { incognitoBtn.style.background = color.surfaceLow; });
        incognitoBtn.addEventListener("mouseleave", function () { incognitoBtn.style.background = "transparent"; });
        incognitoBtn.addEventListener("click", function () {
            if (state.streaming) { return; }
            dismiss();
            require("./chat").toggleIncognito();
        });

        // The shared top row: "New chat" (leading) and the incognito toggle (trailing),
        // each a text button with a leading icon.
        topRow = ui.dom("div", {
            display: "flex", alignItems: "center", justifyContent: "space-between",
            gap: "8px", flex: "0 0 auto", padding: "12px 16px"
        });
        topRow.appendChild(newChatBtn);
        topRow.appendChild(incognitoBtn);

    }   // end signed-in-only top row

    // Health & data + Settings + account are all PINNED at the bottom (flex:0), so
    // only the history list above them scrolls. Groups are compact (tight rows) to
    // fit without needing their own scroll.
    var footer = ui.dom("div", {
        flex: "0 0 auto", borderTop: "1px solid " + color.outlineVar, paddingBottom: "6px"
    });

    if (signedIn) {
        footer.appendChild(navRow(t("careCircle"), "", function () {
            dismiss(); modals.showManageCircleModal();
        }));
        footer.appendChild(navRow(t("ehrConnect"), "", function () {
            dismiss(); require("./ehr").showEhrModal();
        }));
        footer.appendChild(navRow(t("vendorManageTitle"), "", function () {
            dismiss(); require("./vendors").showVendorsModal();
        }));
    }

    // App settings -- what the top-right gear used to hold, now the drawer's own
    // group, and the only group the login screen shows. Each row follows the
    // convention of the rows above: dismiss the drawer first, then open its modal.
    // Language and appearance re-render the whole app afterwards so every string /
    // color picks up the change; mid-stream the render is skipped (it would tear
    // down the in-flight bubble) and the choice lands on the next one. Font size
    // needs no render -- applyFontScale rescales the live UI.
    if (signedIn) { footer.appendChild(drawerDivider()); }

    footer.appendChild(navRow(t("language"), languageLabel(state.language), function () {
        dismiss();
        modals.showLanguageModal(state.language, function (code) {
            state.language = code;
            localStorage.setItem(LANGUAGE_KEY, code);
            i18n.setLang(code);
            applyDirection(code);   // flip <html dir> for Arabic / Hebrew
            if (!state.streaming) { require("./app").render(); }
        });
    }));

    footer.appendChild(navRow(t("fontSize"), t(fontTierLabel(state.fontOffset)), function () {
        dismiss();
        modals.showFontSizeModal(state.fontOffset, function (offset) {
            state.fontOffset = offset;
            localStorage.setItem(FONT_KEY, String(offset));
            applyFontScale(offset);   // live: scales the whole UI immediately
        });
    }));

    footer.appendChild(navRow(t("appearance"), t(themeLabel(state.theme)), function () {
        dismiss();
        modals.showThemeModal(state.theme, function (picked) {
            state.theme = picked;
            localStorage.setItem(THEME_KEY, picked);
            if (!state.streaming) { require("./app").render(); }
        });
    }));

    // Hostname only -- the full address (scheme, port, mount prefix) belongs in the
    // dialog this row opens, not in a one-line hint.
    footer.appendChild(navRow(t("backend"), net.baseHost(), function () {
        dismiss(); modals.showBackendModal();
    }));

    // Account: the signed-in email (centered, muted) above a centered Sign out.
    if (signedIn) {
        footer.appendChild(drawerDivider());

        // Account switcher. The current account (email from the JWT `email` claim)
        // is a tappable row that expands a list of the other signed-in accounts
        // (tap to switch) plus "Switch account" (opens the full login view to sign
        // in another account without dropping the current ones). Several accounts
        // can live in one browser -- see net.js.
        var accounts = net.listAccounts();
        var current = null, others = [];
        accounts.forEach(function (a) { if (a.current) { current = a; } else { others.push(a); } });
        function acctLabel(a) { return (a && a.email) ? a.email : (a ? "#" + a.sub.slice(0, 6) : ""); }

        function acctRowStyle(danger) {
            return {
                display: "flex", alignItems: "center", width: "100%", border: "none",
                background: "transparent", cursor: "pointer", font: "inherit",
                fontSize: "0.85rem", padding: "10px 20px",
                color: danger ? color.error : color.onSurface,
                overflow: "hidden", textOverflow: "ellipsis", whiteSpace: "nowrap"
            };
        }
        function hoverable(el) {
            el.addEventListener("mouseenter", function () { el.style.background = color.surfaceLow; });
            el.addEventListener("mouseleave", function () { el.style.background = "transparent"; });
            return el;
        }

        // Current account + caret; tapping toggles the switcher list below it.
        var acctBtn = ui.dom("button", {
            display: "flex", alignItems: "center", justifyContent: "space-between",
            gap: "8px", width: "100%", border: "none", background: "transparent",
            cursor: "pointer", font: "inherit", padding: "10px 20px", minHeight: "40px"
        }, { type: "button" });
        acctBtn.appendChild(ui.setText(ui.dom("span", {
            fontSize: "0.8rem", color: color.onSurfaceVar, minWidth: "0", flex: "0 1 auto",
            overflow: "hidden", textOverflow: "ellipsis", whiteSpace: "nowrap"
        }), acctLabel(current)));
        var acctCaret = ui.dom("span", {
            display: "inline-flex", alignItems: "center", color: color.onSurfaceVar, flex: "0 0 auto"
        });
        ui.setHTML(acctCaret, icons.CARET_SVG);
        acctBtn.appendChild(acctCaret);

        var switcher = ui.dom("div", { display: "none", flexDirection: "column" });
        others.forEach(function (a) {
            var r = hoverable(ui.dom("button", acctRowStyle(false), { type: "button" }));
            ui.setText(r, acctLabel(a));
            r.addEventListener("click", function () { dismiss(); require("./app").switchAccount(a.sub); });
            switcher.appendChild(r);
        });
        // "Switch account" carries a leading + : the rows above it are switch targets
        // styled identically, so without the glyph this last one reads as one more
        // account rather than the action that adds a new one.
        var addBtn = hoverable(ui.dom("button", acctRowStyle(false), { type: "button" }));
        addBtn.style.gap = "8px";
        var addIcon = ui.dom("span", {
            display: "inline-flex", alignItems: "center", flex: "0 0 auto",
            color: color.onSurfaceVar
        });
        ui.setHTML(addIcon, icons.PLUS_SVG);
        addBtn.appendChild(addIcon);
        addBtn.appendChild(ui.setText(ui.dom("span", { whiteSpace: "nowrap" }),
            t("switchAccount")));
        addBtn.addEventListener("click", function () { dismiss(); require("./app").addAccount(); });
        switcher.appendChild(addBtn);

        acctBtn.addEventListener("click", function () {
            switcher.style.display = (switcher.style.display === "none") ? "flex" : "none";
        });
        footer.appendChild(acctBtn);
        footer.appendChild(switcher);

        // Sign out the current account (confirmed). app.signOut then falls back to
        // another stored account, or the login screen when none remain.
        var signOutBtn = hoverable(ui.dom("button", acctRowStyle(true), { type: "button" }));
        signOutBtn.style.justifyContent = "center";
        signOutBtn.style.minHeight = "40px";
        ui.setText(signOutBtn, t("signOut"));
        signOutBtn.addEventListener("click", function () {
            // Confirm first. The confirm modal (zIndex 1200) sits above the drawer
            // (1100), so keep the drawer open behind it and only dismiss on confirm.
            showConfirmModal(t("signOut"), t("signOutConfirm"), t("signOut"), true, function () {
                dismiss(); require("./app").signOut();
            });
        });
        footer.appendChild(signOutBtn);
    }

    panel.appendChild(header);
    if (topRow) { panel.appendChild(topRow); }
    // The scrolling body is the history list's home, so signed out it is left out
    // altogether rather than added empty: with nothing to scroll, the flex spacer
    // would only push the settings group to the bottom of a blank drawer. Without it
    // the group sits right under the header, which is what a five-row menu should
    // look like.
    if (signedIn) { panel.appendChild(body); }
    panel.appendChild(footer);
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
        ui.clear(historyBox);
        // A lightweight spinner: reuse the streaming-cursor blink on a dot.
        var dot = ui.dom("div", {
            width: "10px", height: "10px", borderRadius: "50%",
            background: color.primary, animation: "mb-blink 1s steps(2, start) infinite"
        });
        historyBox.appendChild(centerBox(dot));
    };

    function showError(msg) {
        ui.clear(historyBox);
        var col = ui.dom("div", {
            display: "flex", flexDirection: "column", alignItems: "center", gap: "10px"
        });
        col.appendChild(ui.setText(ui.dom("div", {
            fontSize: "0.875rem", color: color.error
        }), msg || t("historyLoadFailed")));
        var retry = button(t("retry"), false, { click: load });
        ui.setStyle(retry, { padding: "8px 16px" });
        col.appendChild(retry);
        historyBox.appendChild(centerBox(col));
    };

    function showEmpty() {
        ui.clear(historyBox);
        historyBox.appendChild(centerBox(ui.setText(ui.dom("div", {
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
                // Opening a saved conversation leaves incognito: this is a real,
                // persisted thread, not the ephemeral session. Drop the stash so
                // the (now-abandoned) incognito session isn't restored later.
                state.incognito = false;
                state.incognitoSaved = null;
                // Mirror the opened thread locally (its messages + id) so a reload
                // restores THIS conversation and continues it, matching the screen.
                // Only for an owned/editable thread -- a read-only shared one can't
                // be continued, so it isn't made the local mirror.
                if (!state.readOnly) {
                    var odb = require("./db");
                    odb.saveMessages(state.messages);
                    odb.saveConversationId(state.currentConversationId);
                }
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
            background: strong ? color.selectedBg : color.surfaceLow,
            color: strong ? color.primary : color.onSurfaceVar
        }), text);
    };

    function row(item) {
        var wrap = ui.dom("div", {
            display: "flex", alignItems: "center",
            paddingBlock: "7px", paddingInlineStart: "20px", paddingInlineEnd: "8px",
            borderTop: "1px solid " + color.outlineVar
        });
        wrap.style.borderTopColor = color.outlineVar;

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
            width: "40px", height: "40px", borderRadius: "20px", flex: "0 0 auto",
            display: "flex", alignItems: "center", justifyContent: "center",
            color: color.error
        }, { type: "button", title: t("delete") });
        // Bump the trash glyph up from its 18px default for an easier tap target.
        ui.setHTML(del, TRASH_SVG.replace('width="18" height="18"', 'width="24" height="24"'));
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
        ui.clear(historyBox);
        if (items.length === 0) { showEmpty(); return; }
        var list = ui.dom("div", { padding: "8px 0" });
        for (var i = 0; i < items.length; i ++) {
            list.appendChild(row(items[i]));
        }
        historyBox.appendChild(list);
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

    if (signedIn) { load(); }
};

//----------------------------------------------------------------------------

exports.openHistory = openHistory;

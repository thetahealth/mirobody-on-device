
//----------------------------------------------------------------------------
// The app's dialogs: language picker, font-size picker, backend URL, about,
// the usage/cost summary, and a generic confirm. Each builds a dimmed backdrop
// over the whole page and dismisses on a backdrop click. They mirror the
// Android app's corresponding dialogs.
//----------------------------------------------------------------------------

const ui   = require("./ui");
const net  = require("./net");
const i18n = require("./i18n");
const app  = require("./app");

const config = require("./config");
const color  = config.color;
const state  = config.state;
const LANGUAGES        = config.LANGUAGES;
const FONT_TIERS       = config.FONT_TIERS;
const BASE_URL_PRESETS = config.BASE_URL_PRESETS;

const widgets = require("./widgets");
const button  = widgets.button;
const field   = widgets.field;

var t = i18n.t;

//----------------------------------------------------------------------------

// Language picker modal: a scrollable list of the eight languages, the current
// one tinted/bolded. Picking one calls onPick(code) and dismisses; a backdrop
// click also dismisses. Mirrors the app's LanguageDialog.
function showLanguageModal(current, onPick) {
    var backdrop = ui.dom("div", {
        position: "fixed", inset: "0", background: "rgba(0, 0, 0, 0.4)",
        display: "flex", alignItems: "center", justifyContent: "center",
        padding: "16px", zIndex: "1000"
    });

    var card = ui.dom("div", {
        background: color.background, borderRadius: "14px", padding: "16px",
        width: "100%", maxWidth: "360px", maxHeight: "70vh",
        boxShadow: "0 8px 32px rgba(0, 0, 0, 0.25)",
        display: "flex", flexDirection: "column"
    });
    function dismiss() {
        if (backdrop.parentNode) { backdrop.parentNode.removeChild(backdrop); }
    };
    var head = widgets.modalHeader(t("language"), dismiss);
    head.style.padding = "4px 8px 12px";   // align with the list items' 12px inset
    card.appendChild(head);

    var list = ui.dom("div", { display: "flex", flexDirection: "column", overflowY: "auto" });
    for (var i = 0; i < LANGUAGES.length; i ++) {
        (function (code, label) {
            var selected = code === current;
            var item = ui.setText(ui.dom("button", {
                textAlign: "start", width: "100%", border: "none",
                background: selected ? color.userBubble : "transparent",
                padding: "12px", borderRadius: "10px", cursor: "pointer",
                font: "inherit", fontSize: "1rem",
                fontWeight: selected ? "600" : "400",
                color: selected ? color.primary : color.onSurface
            }, { type: "button" }), label);
            if (!selected) {
                item.addEventListener("mouseenter", function () { item.style.background = color.surfaceLow; });
                item.addEventListener("mouseleave", function () { item.style.background = "transparent"; });
            }
            item.addEventListener("click", function () { onPick(code); dismiss(); });
            list.appendChild(item);
        })(LANGUAGES[i][0], LANGUAGES[i][1]);
    }
    card.appendChild(list);

    backdrop.addEventListener("click", function (evt) {
        if (evt.target === backdrop) { dismiss(); }
    });
    backdrop.appendChild(card);
    document.body.appendChild(backdrop);
};

//----------------------------------------------------------------------------

// Generic confirm modal (title + message + Cancel/confirm). `danger` paints the
// confirm action in the error color. Used by the history delete flow.
function showConfirmModal(title, message, confirmText, danger, onConfirm) {
    var backdrop = ui.dom("div", {
        position: "fixed", inset: "0", background: "rgba(0, 0, 0, 0.4)",
        display: "flex", alignItems: "center", justifyContent: "center",
        padding: "16px", zIndex: "1200"
    });
    var card = ui.dom("div", {
        background: color.background, borderRadius: "14px", padding: "20px 22px",
        width: "100%", maxWidth: "360px",
        boxShadow: "0 8px 32px rgba(0, 0, 0, 0.25)",
        display: "flex", flexDirection: "column", gap: "12px"
    });
    card.appendChild(ui.setText(ui.dom("div", {
        fontSize: "1.05rem", fontWeight: "600", color: color.onSurface
    }), title));
    card.appendChild(ui.setText(ui.dom("div", {
        fontSize: "0.875rem", color: color.onSurfaceVar, lineHeight: "1.5"
    }), message));

    function dismiss() {
        if (backdrop.parentNode) { backdrop.parentNode.removeChild(backdrop); }
    };

    var cancel = button(t("cancel"), false, { click: dismiss });
    var confirm = button(confirmText, true);
    if (danger) { ui.setStyle(confirm, { background: color.error }); }
    confirm.addEventListener("click", function () { dismiss(); onConfirm(); });

    var actions = ui.dom("div", { display: "flex", justifyContent: "flex-end", gap: "8px" });
    actions.appendChild(cancel);
    actions.appendChild(confirm);
    card.appendChild(actions);

    backdrop.addEventListener("click", function (evt) {
        if (evt.target === backdrop) { dismiss(); }
    });
    backdrop.appendChild(card);
    document.body.appendChild(backdrop);
};

//----------------------------------------------------------------------------

// Simple alert modal: title + message + a single OK button (plus a backdrop
// click). For pure notifications where a Cancel would be meaningless (unlike
// showConfirmModal's two-button decision). onOk (optional) runs after dismiss.
function showAlertModal(title, message, okText, onOk) {
    var backdrop = ui.dom("div", {
        position: "fixed", inset: "0", background: "rgba(0, 0, 0, 0.4)",
        display: "flex", alignItems: "center", justifyContent: "center",
        padding: "16px", zIndex: "1200"
    });
    var card = ui.dom("div", {
        background: color.background, borderRadius: "14px", padding: "20px 22px",
        width: "100%", maxWidth: "360px",
        boxShadow: "0 8px 32px rgba(0, 0, 0, 0.25)",
        display: "flex", flexDirection: "column", gap: "12px"
    });
    card.appendChild(ui.setText(ui.dom("div", {
        fontSize: "1.05rem", fontWeight: "600", color: color.onSurface
    }), title));
    card.appendChild(ui.setText(ui.dom("div", {
        fontSize: "0.875rem", color: color.onSurfaceVar, lineHeight: "1.5"
    }), message));

    function dismiss() {
        if (backdrop.parentNode) { backdrop.parentNode.removeChild(backdrop); }
    };

    var ok = button(okText || t("ehrOk"), true);
    ok.addEventListener("click", function () { dismiss(); if (typeof onOk === "function") { onOk(); } });
    var actions = ui.dom("div", { display: "flex", justifyContent: "flex-end" });
    actions.appendChild(ok);
    card.appendChild(actions);

    backdrop.addEventListener("click", function (evt) {
        if (evt.target === backdrop) { dismiss(); }
    });
    backdrop.appendChild(card);
    document.body.appendChild(backdrop);
};

//----------------------------------------------------------------------------

// Backend base-URL modal: an editable field (with the preset origins as
// suggestions), validated as an http(s) URL or left blank to use this server.
// Saving routes all later requests through net.setBaseUrl and reloads the
// provider list from the new backend. Mirrors the app's BaseUrlDialog.
function showBackendModal() {
    var backdrop = ui.dom("div", {
        position: "fixed", inset: "0", background: "rgba(0, 0, 0, 0.4)",
        display: "flex", alignItems: "center", justifyContent: "center",
        padding: "16px", zIndex: "1000"
    });
    var card = ui.dom("div", {
        background: color.background, borderRadius: "14px", padding: "20px 22px",
        width: "100%", maxWidth: "420px",
        boxShadow: "0 8px 32px rgba(0, 0, 0, 0.25)",
        display: "flex", flexDirection: "column", gap: "12px"
    });

    card.appendChild(widgets.modalHeader(t("backendUrl"), dismiss));
    card.appendChild(ui.setText(ui.dom("div", {
        fontSize: "0.875rem", color: color.onSurfaceVar, lineHeight: "1.4"
    }), t("backendSubtitle")));

    var listId = "mb-backend-presets";
    var input = field({ type: "text", placeholder: net.appBase() || window.location.origin, list: listId, autocomplete: "off" });
    input.value = net.getBaseUrl();
    var datalist = ui.dom("datalist", null, { id: listId });
    for (var i = 0; i < BASE_URL_PRESETS.length; i ++) {
        datalist.appendChild(ui.dom("option", null, { value: BASE_URL_PRESETS[i] }));
    }
    card.appendChild(input);
    card.appendChild(datalist);

    var error = ui.dom("div", {
        fontSize: "0.8rem", color: color.error, minHeight: "1em", display: "none"
    });
    card.appendChild(error);
    card.appendChild(ui.setText(ui.dom("div", {
        fontSize: "0.8rem", color: color.onSurfaceVar
    }), t("backendHint")));

    function dismiss() {
        if (backdrop.parentNode) { backdrop.parentNode.removeChild(backdrop); }
    };

    var cancel = button(t("cancel"), false, { click: dismiss });
    var save = button(t("save"), true);
    save.addEventListener("click", function () {
        var v = input.value.trim().replace(/\/+$/, "");
        if (v && !/^https?:\/\/\S+$/i.test(v)) {
            ui.setText(error, t("invalidUrl"));
            ui.setStyle(error, { display: "block" });
            return;
        }
        net.setBaseUrl(v);
        dismiss();
        // Re-fetch providers from the new backend; the existing session token
        // rides along and, if the new backend rejects it, a 401 signs out.
        state.providers = [];
        if (!state.streaming) { app.render(); }
        app.loadProviders();
    });

    var actions = ui.dom("div", { display: "flex", justifyContent: "flex-end", gap: "8px" });
    actions.appendChild(cancel);
    actions.appendChild(save);
    card.appendChild(actions);

    backdrop.addEventListener("click", function (evt) {
        if (evt.target === backdrop) { dismiss(); }
    });
    backdrop.appendChild(card);
    document.body.appendChild(backdrop);
};

//----------------------------------------------------------------------------

// Font-size picker modal: a five-step slider with tier labels. Dragging applies
// the scale live (via onChange, which scales the root font and persists) so the
// whole UI -- including this modal -- resizes as a preview. Mirrors the app's
// FontSizeDialog.
function showFontSizeModal(current, onChange) {
    var backdrop = ui.dom("div", {
        position: "fixed", inset: "0", background: "rgba(0, 0, 0, 0.4)",
        display: "flex", alignItems: "center", justifyContent: "center",
        padding: "16px", zIndex: "1000"
    });
    var card = ui.dom("div", {
        background: color.background, borderRadius: "14px", padding: "20px 22px",
        width: "100%", maxWidth: "360px",
        boxShadow: "0 8px 32px rgba(0, 0, 0, 0.25)",
        display: "flex", flexDirection: "column", gap: "16px"
    });
    function dismiss() {
        if (backdrop.parentNode) { backdrop.parentNode.removeChild(backdrop); }
    };
    card.appendChild(widgets.modalHeader(t("fontSize"), dismiss));

    function tierIndex(offset) {
        for (var i = 0; i < FONT_TIERS.length; i ++) {
            if (FONT_TIERS[i][0] === offset) { return i; }
        }
        return 2; // Normal
    };
    var idx = tierIndex(current);

    var slider = ui.dom("input", {
        width: "100%", accentColor: color.primary, cursor: "pointer"
    }, { type: "range", min: "0", max: String(FONT_TIERS.length - 1), step: "1", value: String(idx) });

    var labels = ui.dom("div", { display: "flex", justifyContent: "space-between" });
    var labelEls = [];
    for (var i = 0; i < FONT_TIERS.length; i ++) {
        var lbl = ui.setText(ui.dom("span", {
            fontSize: "0.7rem", textAlign: "center", flex: "1 1 0"
        }), t(FONT_TIERS[i][1]));
        labelEls.push(lbl);
        labels.appendChild(lbl);
    }
    function paint() {
        for (var j = 0; j < labelEls.length; j ++) {
            labelEls[j].style.color = (j === idx) ? color.primary : color.onSurfaceVar;
            labelEls[j].style.fontWeight = (j === idx) ? "600" : "400";
        }
    };
    paint();

    slider.addEventListener("input", function () {
        idx = parseInt(slider.value, 10) || 0;
        onChange(FONT_TIERS[idx][0]);
        paint();
    });

    card.appendChild(slider);
    card.appendChild(labels);

    backdrop.addEventListener("click", function (evt) {
        if (evt.target === backdrop) { dismiss(); }
    });
    backdrop.appendChild(card);
    document.body.appendChild(backdrop);
};

//----------------------------------------------------------------------------

// Show the usage/cost summary in a centered modal. Dismissed by the Close
// button or a click on the dim backdrop (clicks on the card itself don't
// close). Appended to document.body so it overlays the whole app.
function showCostModal(cost) {
    var backdrop = ui.dom("div", {
        position       : "fixed",
        inset          : "0",
        background     : "rgba(0, 0, 0, 0.4)",
        display        : "flex",
        alignItems     : "center",
        justifyContent : "center",
        padding        : "16px",
        zIndex         : "1000"
    });

    var card = ui.dom("div", {
        background    : color.background,
        borderRadius  : "14px",
        padding       : "20px 22px",
        width         : "100%",
        maxWidth      : "420px",
        boxShadow     : "0 8px 32px rgba(0, 0, 0, 0.25)",
        display       : "flex",
        flexDirection : "column",
        gap           : "14px"
    });
    function dismiss() {
        if (backdrop.parentNode) { backdrop.parentNode.removeChild(backdrop); }
    };
    card.appendChild(widgets.modalHeader(t("statsTitle"), dismiss));
    // One row per stat: label flush left, value flush right (space-between).
    var rows = ui.dom("div", {
        display       : "flex",
        flexDirection : "column",
        gap           : "6px",
        fontSize      : "0.875rem",
        lineHeight    : "1.7"
    });
    function statRow(label, value) {
        var r = ui.dom("div", {
            display        : "flex",
            justifyContent : "space-between",
            alignItems     : "baseline",
            gap            : "16px"
        });
        r.appendChild(ui.setText(ui.dom("span", {
            flex  : "0 0 auto",
            color : color.onSurfaceVar
        }), label));
        r.appendChild(ui.setText(ui.dom("span", {
            flex      : "1 1 auto",
            textAlign : "end",
            wordBreak : "break-word",
            color     : color.onSurface
        }), value));
        rows.appendChild(r);
    };
    statRow(t("statsModel"),       String(cost.model || ""));
    statRow(t("statsInput"),       String(cost.input_tokens || 0));
    statRow(t("statsOutput"),      String(cost.output_tokens || 0));
    statRow(t("statsTotalTokens"), String(cost.total_tokens || 0));
    // Round to 4 decimals (tenth-of-a-cent) so the figure reads as money rather
    // than a raw float printed to 6+ digits.
    statRow(t("statsTotalCost"),   "$" + (Number(cost.total_cost) || 0).toFixed(4));
    card.appendChild(rows);

    backdrop.addEventListener("click", function (evt) {
        if (evt.target === backdrop) { dismiss(); }
    });

    backdrop.appendChild(card);
    document.body.appendChild(backdrop);
};

//----------------------------------------------------------------------------

// Care-circle management: list my circle's members (invite/remove) and the
// invitations sent *to* me (accept/decline). Backed by /api/circle/*.
function showManageCircleModal() {
    var backdrop = ui.dom("div", {
        position: "fixed", inset: "0", background: "rgba(0, 0, 0, 0.4)",
        display: "flex", alignItems: "center", justifyContent: "center",
        padding: "16px", zIndex: "1000"
    });
    var card = ui.dom("div", {
        background: color.background, borderRadius: "14px", padding: "20px 22px",
        width: "100%", maxWidth: "460px", maxHeight: "80vh",
        boxShadow: "0 8px 32px rgba(0, 0, 0, 0.25)",
        display: "flex", flexDirection: "column", gap: "12px"
    });
    function dismiss() { if (backdrop.parentNode) { backdrop.parentNode.removeChild(backdrop); } }
    card.appendChild(widgets.modalHeader(t("circleManageTitle"), dismiss));

    var bodyWrap = ui.dom("div", {
        flex: "1 1 auto", overflowY: "auto", minHeight: "0",
        display: "flex", flexDirection: "column", gap: "2px"
    });
    card.appendChild(bodyWrap);
    var status = ui.dom("div", { fontSize: "0.8rem", color: color.onSurfaceVar, minHeight: "1em" });

    function sectionHeader(text) {
        return ui.setText(ui.dom("div", {
            fontSize: "0.72rem", fontWeight: "600", letterSpacing: "0.04em",
            color: color.onSurfaceVar, marginTop: "8px", textTransform: "uppercase"
        }), text);
    }
    function badge(text, strong) {
        return ui.setText(ui.dom("span", {
            fontSize: "0.72rem", padding: "2px 8px", borderRadius: "10px", flex: "0 0 auto",
            background: strong ? color.userBubble : color.surfaceLow,
            color: strong ? color.primary : color.onSurfaceVar, whiteSpace: "nowrap"
        }), text);
    }
    function rowBox() {
        return ui.dom("div", {
            display: "flex", alignItems: "center", gap: "8px", minHeight: "52px",
            padding: "6px 4px", borderBottom: "1px solid " + color.outlineVar
        });
    }
    function emailText(s) {
        return ui.setText(ui.dom("div", {
            flex: "1 1 auto", minWidth: "0", fontSize: "0.95rem", lineHeight: "1.4", color: color.onSurface,
            padding: "0 2px", overflow: "hidden", textOverflow: "ellipsis", whiteSpace: "nowrap"
        }), s);
    }
    function smallBtn(label, primary, onClick, danger) {
        var b = button(label, primary, { click: onClick });
        ui.setStyle(b, { padding: "6px 12px", fontSize: "0.8rem", flex: "0 0 auto" });
        if (danger) { ui.setStyle(b, { background: color.error, color: "#fff", border: "none" }); }
        return b;
    }

    function memberRow(m, circleId, myRole) {
        var admin = myRole >= 1;                    // Owner or Maintainer
        var row = rowBox();
        if (m.role === 2) {            // the circle's Owner
            row.appendChild(emailText(m.email || ("#" + m.member)));
            if (m.me) { row.appendChild(badge(t("circleYou"), true)); }
            row.appendChild(badge(t("circleOwner"), false));
            return row;
        }
        // Identity. Admins get an editable nickname (their label, e.g. "Mom") over
        // the email; a plain member sees it read-only (nickname or email).
        var left = ui.dom("div", { flex: "1 1 auto", minWidth: "0", display: "flex", flexDirection: "column", gap: "1px" });
        if (admin) {
            var nick = ui.dom("input", {
                width: "100%", boxSizing: "border-box", font: "inherit", fontSize: "0.95rem", lineHeight: "1.4",
                color: color.onSurface, background: "transparent", border: "1px solid transparent",
                borderRadius: "8px", padding: "1px 2px"
            }, { type: "text", placeholder: t("circleNicknamePlaceholder") });
            nick.value = m.nickname || "";
            nick.addEventListener("focus", function () { nick.style.borderColor = color.outlineVar; });
            nick.addEventListener("blur", function () {
                nick.style.borderColor = "transparent";
                var v = nick.value.trim();
                if (v === (m.nickname || "")) { nick.value = m.nickname || ""; return; }
                net.post("/api/circle/nickname", { care_circle_id: circleId, member: m.member, nickname: v },
                         function () { m.nickname = v; }, function () { nick.value = m.nickname || ""; });
            });
            nick.addEventListener("keydown", function (e) { if (e.key === "Enter") { e.preventDefault(); nick.blur(); } });
            left.appendChild(nick);
        } else {
            left.appendChild(ui.setText(ui.dom("div", {
                fontSize: "0.95rem", lineHeight: "1.4", color: color.onSurface, padding: "1px 2px",
                overflow: "hidden", textOverflow: "ellipsis", whiteSpace: "nowrap"
            }), m.nickname || m.email || ("#" + m.member)));
        }
        // Email subtitle: always for admins (the name line is the nickname field);
        // for members only when a nickname is already the primary line.
        if (admin || m.nickname) {
            left.appendChild(ui.setText(ui.dom("div", {
                fontSize: "0.75rem", color: color.onSurfaceVar, padding: "0 3px",
                overflow: "hidden", textOverflow: "ellipsis", whiteSpace: "nowrap"
            }), m.email || ("#" + m.member)));
        }
        row.appendChild(left);

        if (m.me) { row.appendChild(badge(t("circleYou"), true)); }
        if (m.status !== "accepted") { row.appendChild(badge(t("circlePending"), false)); }

        if (myRole === 2) {
            // Owner: promote/demote this member between Member and Maintainer.
            var roleSel = ui.dom("select", {
                font: "inherit", fontSize: "0.78rem", padding: "6px 8px", borderRadius: "8px",
                border: "1px solid " + color.outlineVar, background: color.background,
                color: color.onSurface, cursor: "pointer", flex: "0 0 auto"
            });
            [["member", "circleRoleMember"], ["maintainer", "circleRoleMaintainer"]].forEach(function (o) {
                var op = ui.dom("option", null, { value: o[0] }); ui.setText(op, t(o[1])); roleSel.appendChild(op);
            });
            roleSel.value = (m.role === 1) ? "maintainer" : "member";
            roleSel.addEventListener("change", function () {
                net.post("/api/circle/role", { care_circle_id: circleId, member: m.member, role: roleSel.value },
                         function () { reload(); }, function () { reload(); });
            });
            row.appendChild(roleSel);
        } else if (m.role === 1) {
            row.appendChild(badge(t("circleMaintainer"), true));   // a fellow maintainer
        }
        // Remove: owner removes any non-owner; a maintainer removes Members only.
        if (myRole === 2 || (myRole === 1 && m.role === 0)) {
            row.appendChild(smallBtn(t("circleRemoveBtn"), false, function () {
                var who = m.nickname || m.email || ("#" + m.member);
                showConfirmModal(t("circleRemoveTitle"), t("circleRemoveConfirm", who),
                                 t("circleRemoveBtn"), true, function () {
                    net.post("/api/circle/remove", { member: m.member, care_circle_id: circleId },
                             function () { reload(); }, function () {});
                });
            }, true));
        }
        return row;
    }

    // A per-circle invite row (email + Invite), scoped to one circle id.
    function inviteInto(circleId) {
        var wrap = ui.dom("div", { display: "flex", gap: "8px", alignItems: "stretch", marginTop: "12px" });
        var em = field({ type: "email", placeholder: t("circleInvitePlaceholder"), autocomplete: "off" });
        ui.setStyle(em, { flex: "1 1 auto", padding: "11px 14px" });
        var btn = button(t("circleInviteBtn"), true);
        ui.setStyle(btn, { flex: "0 0 auto" });
        function go() {
            var v = em.value.trim();
            if (!v) { ui.setText(status, t("circleEmailRequired")); ui.setStyle(status, { color: color.error }); return; }
            net.post("/api/circle/invite", { email: v, care_circle_id: circleId }, function () {
                ui.setText(status, t("circleInvited", v)); ui.setStyle(status, { color: color.onSurfaceVar });
                reload();
            }, function (msg) {
                ui.setText(status, ui.isString(msg) ? msg : t("circleInviteFailed"));
                ui.setStyle(status, { color: color.error });
            });
        }
        btn.addEventListener("click", go);
        em.addEventListener("keydown", function (e) { if (e.key === "Enter") { e.preventDefault(); go(); } });
        wrap.appendChild(em); wrap.appendChild(btn);
        return wrap;
    }

    // Per-circle "Share my health data" level. health_access lives on each
    // membership, so I can share with one circle but not another; this sets my own
    // membership in circle c (my_health_access is my row's value, from the server).
    function healthRowFor(c) {
        var lvl = c.my_health_access || 0;
        var prev = lvl >= 2 ? "edit" : (lvl >= 1 ? "view" : "off");

        var row = ui.dom("div", {
            display: "flex", alignItems: "center", gap: "8px", minHeight: "52px",
            padding: "6px 4px", borderBottom: "1px solid " + color.outlineVar
        });
        row.appendChild(ui.setText(ui.dom("div", {
            flex: "1 1 auto", minWidth: "0", fontSize: "0.85rem", lineHeight: "1.4", color: color.onSurfaceVar,
            padding: "0 2px"
        }), t("healthShareLabel")));
        var sel = ui.dom("select", {
            font: "inherit", fontSize: "0.8rem", padding: "4px 6px", borderRadius: "8px",
            border: "1px solid " + color.outlineVar, background: color.background,
            color: color.onSurface, cursor: "pointer", flex: "0 0 auto"
        });
        [["off", "healthShareOff"], ["view", "healthShareViewOpt"], ["edit", "healthShareEditOpt"]].forEach(function (o) {
            var op = ui.dom("option", null, { value: o[0] }); ui.setText(op, t(o[1])); sel.appendChild(op);
        });
        sel.value = prev;
        sel.addEventListener("change", function () {
            var v = sel.value;
            net.post("/api/circle/health-sharing", { access: v, care_circle_id: c.circle_id },
                     function () { prev = v; }, function () { sel.value = prev; });   // revert on failure
        });
        row.appendChild(sel);
        return row;
    }

    // One circle I administer, as a collapsible accordion item. The clickable
    // header (chevron + name + role/Delete) toggles the body via onToggle; only
    // one circle is open at a time. Owner gets an editable name + Delete + role
    // controls; a maintainer sees a static name badged "maintainer". Returns
    // { el, setOpen } so the caller can drive which item is expanded.
    function circleBlock(c, onToggle) {
        var owner = (c.my_role === 2);
        var block = ui.dom("div", {
            border: "1px solid " + color.outlineVar, borderRadius: "12px",
            marginTop: "10px", overflow: "hidden"
        });
        var head = ui.dom("div", {
            display: "flex", alignItems: "center", gap: "8px", minHeight: "48px",
            padding: "6px 12px", cursor: "pointer"
        });
        head.addEventListener("click", function () { onToggle(); });
        var chevron = ui.setText(ui.dom("span", {
            flex: "0 0 auto", fontSize: "0.95rem", lineHeight: "1", color: color.primary,
            width: "20px", textAlign: "center"
        }), "▶");

        head.appendChild(chevron);
        if (owner) {
            var nameInput = ui.dom("input", {
                flex: "1 1 auto", minWidth: "0", font: "inherit", fontSize: "0.98rem", fontWeight: "600",
                color: color.onSurface, background: "transparent", border: "1px solid transparent",
                borderRadius: "8px", padding: "4px 2px"
            }, { type: "text" });
            nameInput.value = c.name || "";
            // Editing the name must not toggle the accordion.
            nameInput.addEventListener("click", function (e) { e.stopPropagation(); });
            nameInput.addEventListener("focus", function () { nameInput.style.borderColor = color.outlineVar; });
            nameInput.addEventListener("blur", function () {
                nameInput.style.borderColor = "transparent";
                var nm = nameInput.value.trim();
                if (!nm || nm === (c.name || "")) { nameInput.value = c.name || ""; return; }
                net.post("/api/circle/rename", { care_circle_id: c.circle_id, name: nm },
                         function () { c.name = nm; }, function () { nameInput.value = c.name || ""; });
            });
            nameInput.addEventListener("keydown", function (e) { if (e.key === "Enter") { e.preventDefault(); nameInput.blur(); } });
            head.appendChild(nameInput);
            var del = button(t("circleDeleteCircleBtn"), true, { click: function () {
                showConfirmModal(t("circleDeleteTitle"), t("circleDeleteConfirm", c.name || ""),
                                 t("circleDeleteBtn"), true, function () {
                    net.post("/api/circle/delete", { care_circle_id: c.circle_id }, function () { reload(); }, function () {});
                });
            } });
            ui.setStyle(del, { flex: "0 0 auto", background: color.error, color: "#fff", border: "none" });
            del.addEventListener("click", function (e) { e.stopPropagation(); });
            head.appendChild(del);
        } else {
            head.appendChild(ui.setText(ui.dom("div", {
                flex: "1 1 auto", minWidth: "0", fontSize: "0.98rem", fontWeight: "600", color: color.onSurface,
                padding: "4px 2px", overflow: "hidden", textOverflow: "ellipsis", whiteSpace: "nowrap"
            }), c.name || ""));
            // My role in this circle: maintainer (admin) or plain member.
            head.appendChild(badge(c.my_role === 1 ? t("circleMaintainer") : t("circleRoleMember"), c.my_role === 1));
        }
        block.appendChild(head);

        var body = ui.dom("div", { display: "none", flexDirection: "column", padding: "0 12px 10px" });
        body.appendChild(healthRowFor(c));
        var ms = (c.members instanceof Array) ? c.members : [];
        for (var i = 0; i < ms.length; i ++) { body.appendChild(memberRow(ms[i], c.circle_id, c.my_role)); }
        if (c.my_role >= 1) { body.appendChild(inviteInto(c.circle_id)); }   // admins only
        block.appendChild(body);

        function setOpen(open) {
            body.style.display = open ? "flex" : "none";
            head.style.borderBottom = open ? "1px solid " + color.outlineVar : "none";
            ui.setText(chevron, open ? "▼" : "▶");
        }
        return { el: block, setOpen: setOpen };
    }

    function inviteRow(inv) {
        var row = rowBox();
        var who = inv.owner_email || inv.circle_name || "";
        var label = (inv.circle_name && inv.owner_email)
            ? (inv.owner_email + " · " + inv.circle_name)
            : who;
        row.appendChild(emailText(label));
        row.appendChild(smallBtn(t("circleAcceptBtn"), true, function () {
            net.post("/api/circle/accept", { token: inv.token }, function () { reload(); }, function () {});
        }));
        row.appendChild(smallBtn(t("circleDeclineBtn"), false, function () {
            net.post("/api/circle/decline", { token: inv.token }, function () { reload(); }, function () {});
        }));
        return row;
    }

    function renderData(data) {
        ui.clear(bodyWrap);
        var circles = (data && data.circles instanceof Array) ? data.circles : [];
        var invites = (data && data.invites instanceof Array) ? data.invites : [];

        if (circles.length === 0) {
            bodyWrap.appendChild(ui.setText(ui.dom("div", {
                fontSize: "0.85rem", color: color.onSurfaceVar, padding: "6px 2px"
            }), t("circleNoCircles")));
        }
        // Accordion: one circle expanded at a time (the first by default); clicking
        // an open header collapses it.
        var ctrls = [];
        var openIdx = circles.length ? 0 : -1;
        function apply() { for (var k = 0; k < ctrls.length; k ++) { ctrls[k].setOpen(k === openIdx); } }
        for (var i = 0; i < circles.length; i ++) {
            (function (idx) {
                var cb = circleBlock(circles[idx], function () { openIdx = (openIdx === idx) ? -1 : idx; apply(); });
                ctrls.push(cb);
                bodyWrap.appendChild(cb.el);
            })(i);
        }
        apply();

        if (invites.length) {
            bodyWrap.appendChild(sectionHeader(t("circleInvitesLabel")));
            for (var j = 0; j < invites.length; j ++) { bodyWrap.appendChild(inviteRow(invites[j])); }
        }
    }
    function reload() {
        net.get("/api/circle/members", renderData, function () {
            ui.clear(bodyWrap);
            bodyWrap.appendChild(ui.setText(ui.dom("div", {
                fontSize: "0.85rem", color: color.error
            }), t("circleLoadFailed")));
        });
    }

    card.appendChild(status);

    // Footer: create a new circle and close, on one line.
    var footer = ui.dom("div", { display: "flex", gap: "8px", alignItems: "center" });
    var newName = field({ type: "text", placeholder: t("circleNewNamePlaceholder"), autocomplete: "off" });
    ui.setStyle(newName, { flex: "1 1 auto", padding: "11px 14px" });
    var createBtn = button(t("circleCreateBtn"), true);
    ui.setStyle(createBtn, { flex: "0 0 auto" });
    function doCreate() {
        net.post("/api/circle/create", { name: newName.value.trim() }, function () {
            newName.value = ""; ui.setText(status, ""); reload();
        }, function () { ui.setText(status, t("circleCreateFailed")); ui.setStyle(status, { color: color.error }); });
    }
    createBtn.addEventListener("click", doCreate);
    newName.addEventListener("keydown", function (e) { if (e.key === "Enter") { e.preventDefault(); doCreate(); } });
    footer.appendChild(newName);
    footer.appendChild(createBtn);
    card.appendChild(footer);

    backdrop.addEventListener("click", function (evt) { if (evt.target === backdrop) { dismiss(); } });
    backdrop.appendChild(card);
    document.body.appendChild(backdrop);
    reload();
};

//----------------------------------------------------------------------------

// Share a conversation with accepted care-circle members: a checkbox + access
// (View/Edit) per member, pre-checked from the current shares. Save diffs the
// selection against what was shared (/api/conversation/share | unshare).
function showShareModal(conversationId) {
    var backdrop = ui.dom("div", {
        position: "fixed", inset: "0", background: "rgba(0, 0, 0, 0.4)",
        display: "flex", alignItems: "center", justifyContent: "center",
        padding: "16px", zIndex: "1000"
    });
    var card = ui.dom("div", {
        background: color.background, borderRadius: "14px", padding: "20px 22px",
        width: "100%", maxWidth: "420px", maxHeight: "80vh",
        boxShadow: "0 8px 32px rgba(0, 0, 0, 0.25)",
        display: "flex", flexDirection: "column", gap: "12px"
    });
    function dismiss() { if (backdrop.parentNode) { backdrop.parentNode.removeChild(backdrop); } }
    card.appendChild(widgets.modalHeader(t("shareTitle"), dismiss));
    function mount() {
        backdrop.addEventListener("click", function (evt) { if (evt.target === backdrop) { dismiss(); } });
        backdrop.appendChild(card);
        document.body.appendChild(backdrop);
    }

    // No conversation yet: explain and bail with a single Close.
    if (!conversationId) {
        card.appendChild(ui.setText(ui.dom("div", {
            fontSize: "0.875rem", color: color.onSurfaceVar, lineHeight: "1.5"
        }), t("shareNoConversation")));
        mount();
        return;
    }

    var body = ui.dom("div", {
        flex: "1 1 auto", overflowY: "auto", minHeight: "0",
        display: "flex", flexDirection: "column", gap: "2px"
    });
    card.appendChild(body);

    var rows = [];   // { member, email, checkbox, select, wasShared }

    var save = button(t("shareSave"), true);
    save.addEventListener("click", function () {
        for (var i = 0; i < rows.length; i ++) {
            var r = rows[i];
            if (r.checkbox.checked) {
                net.post("/api/conversation/share",
                    { conversation_id: conversationId, members: [r.member], access: r.select.value },
                    null, null);
            } else if (r.wasShared) {
                net.post("/api/conversation/unshare",
                    { conversation_id: conversationId, member: r.member }, null, null);
            }
        }
        dismiss();
    });
    var cancel = button(t("cancel"), false, { click: dismiss });
    var actions = ui.dom("div", { display: "flex", justifyContent: "flex-end", gap: "8px" });
    actions.appendChild(cancel);
    actions.appendChild(save);

    function renderMembers(members, sharedMap) {
        ui.clear(body);
        rows = [];
        var others = members.filter(function (m) { return m.role !== 1 && m.status === "accepted"; });
        if (others.length === 0) {
            body.appendChild(ui.setText(ui.dom("div", {
                fontSize: "0.85rem", color: color.onSurfaceVar, padding: "6px 2px"
            }), t("shareNoMembers")));
            save.style.display = "none";
            return;
        }
        for (var i = 0; i < others.length; i ++) {
            (function (m) {
                var existing = sharedMap[m.email];
                var rowEl = ui.dom("div", {
                    display: "flex", alignItems: "center", gap: "10px",
                    padding: "8px 2px", borderBottom: "1px solid " + color.outlineVar
                });
                var cb = ui.dom("input", { width: "18px", height: "18px", cursor: "pointer", accentColor: color.primary },
                                { type: "checkbox" });
                if (existing) { cb.checked = true; }
                rowEl.appendChild(cb);
                rowEl.appendChild(ui.setText(ui.dom("div", {
                    flex: "1 1 auto", minWidth: "0", fontSize: "0.95rem", color: color.onSurface,
                    overflow: "hidden", textOverflow: "ellipsis", whiteSpace: "nowrap"
                }), m.email || ("#" + m.member)));
                var sel = ui.dom("select", {
                    font: "inherit", fontSize: "0.8rem", padding: "4px 6px",
                    borderRadius: "8px", border: "1px solid " + color.outlineVar,
                    background: color.background, color: color.onSurface, cursor: "pointer"
                });
                var ov = ui.dom("option", null, { value: "view" });  ui.setText(ov, t("shareAccessView"));
                var oe = ui.dom("option", null, { value: "edit" });  ui.setText(oe, t("shareAccessEdit"));
                sel.appendChild(ov); sel.appendChild(oe);
                sel.value = existing === "edit" ? "edit" : "view";
                sel.disabled = !cb.checked;
                cb.addEventListener("change", function () { sel.disabled = !cb.checked; });
                rowEl.appendChild(sel);
                body.appendChild(rowEl);
                rows.push({ member: m.member, email: m.email, checkbox: cb, select: sel, wasShared: !!existing });
            })(others[i]);
        }
    }

    card.appendChild(actions);
    mount();

    // Load members, then the current shares, then render. Members are flattened
    // across all my circles (deduped) -- being in any one circle is shareable.
    net.get("/api/circle/members", function (md) {
        var circles = (md && md.circles instanceof Array) ? md.circles : [];
        var seen = {}, members = [];
        for (var ci = 0; ci < circles.length; ci ++) {
            var ms = circles[ci].members || [];
            for (var mi = 0; mi < ms.length; mi ++) {
                var m = ms[mi];
                if (m.role === 1 || m.status !== "accepted" || seen[m.email]) continue;
                seen[m.email] = true;
                members.push(m);
            }
        }
        net.get("/api/conversation/shares?id=" + encodeURIComponent(conversationId), function (sd) {
            var map = {};
            var shares = (sd && sd.shares instanceof Array) ? sd.shares : [];
            for (var i = 0; i < shares.length; i ++) { map[shares[i].email] = shares[i].access || "view"; }
            renderMembers(members, map);
        }, function () { renderMembers(members, {}); });
    }, function () { renderMembers([], {}); });
};

//----------------------------------------------------------------------------

exports.showManageCircleModal = showManageCircleModal;
exports.showShareModal    = showShareModal;
exports.showLanguageModal = showLanguageModal;
exports.showConfirmModal  = showConfirmModal;
exports.showAlertModal    = showAlertModal;
exports.showBackendModal  = showBackendModal;
exports.showFontSizeModal = showFontSizeModal;
exports.showCostModal     = showCostModal;


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
const APP_VERSION      = config.APP_VERSION;

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
    card.appendChild(ui.setText(ui.dom("div", {
        fontSize: "1.05rem", fontWeight: "600", color: color.onSurface,
        padding: "4px 8px 12px"
    }), t("language")));

    function dismiss() {
        if (backdrop.parentNode) { backdrop.parentNode.removeChild(backdrop); }
    };

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

// About modal: the app name and the build version. Mirrors the app's
// AboutDialog. Dismissed by Close or a backdrop click.
function showAboutModal() {
    var backdrop = ui.dom("div", {
        position: "fixed", inset: "0", background: "rgba(0, 0, 0, 0.4)",
        display: "flex", alignItems: "center", justifyContent: "center",
        padding: "16px", zIndex: "1000"
    });
    var card = ui.dom("div", {
        background: color.background, borderRadius: "14px", padding: "20px 22px",
        width: "100%", maxWidth: "360px",
        boxShadow: "0 8px 32px rgba(0, 0, 0, 0.25)",
        display: "flex", flexDirection: "column", gap: "14px"
    });

    card.appendChild(ui.setText(ui.dom("div", {
        fontSize: "1.05rem", fontWeight: "600", color: color.onSurface
    }), t("about")));

    var body = ui.dom("div", { display: "flex", flexDirection: "column", gap: "4px" });
    body.appendChild(ui.setText(ui.dom("div", {
        fontSize: "1rem", fontWeight: "500", color: color.onSurface
    }), "Mirobody"));
    body.appendChild(ui.setText(ui.dom("div", {
        fontSize: "0.875rem", color: color.onSurfaceVar
    }), t("version", APP_VERSION)));
    card.appendChild(body);

    function dismiss() {
        if (backdrop.parentNode) { backdrop.parentNode.removeChild(backdrop); }
    };
    var actions = ui.dom("div", { display: "flex", justifyContent: "flex-end" });
    actions.appendChild(button(t("close"), true, { click: dismiss }));
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

    card.appendChild(ui.setText(ui.dom("div", {
        fontSize: "1.05rem", fontWeight: "600", color: color.onSurface
    }), t("backendUrl")));
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
    card.appendChild(ui.setText(ui.dom("div", {
        fontSize: "1.05rem", fontWeight: "600", color: color.onSurface
    }), t("fontSize")));

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

    function dismiss() {
        if (backdrop.parentNode) { backdrop.parentNode.removeChild(backdrop); }
    };
    var actions = ui.dom("div", { display: "flex", justifyContent: "flex-end" });
    actions.appendChild(button(t("close"), true, { click: dismiss }));
    card.appendChild(actions);

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
    card.appendChild(ui.setText(ui.dom("div", {
        fontSize   : "1.05rem",
        fontWeight : "600",
        color      : color.onSurface
    }), t("statsTitle")));
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
    statRow(t("statsTotalCost"),   "$" + (cost.total_cost || 0));
    card.appendChild(rows);

    function dismiss() {
        if (backdrop.parentNode) { backdrop.parentNode.removeChild(backdrop); }
    };

    var actions = ui.dom("div", { display: "flex", justifyContent: "flex-end" });
    actions.appendChild(button(t("close"), true, { click: dismiss }));
    card.appendChild(actions);

    backdrop.addEventListener("click", function (evt) {
        if (evt.target === backdrop) { dismiss(); }
    });

    backdrop.appendChild(card);
    document.body.appendChild(backdrop);
};

//----------------------------------------------------------------------------

exports.showLanguageModal = showLanguageModal;
exports.showConfirmModal  = showConfirmModal;
exports.showAboutModal    = showAboutModal;
exports.showBackendModal  = showBackendModal;
exports.showFontSizeModal = showFontSizeModal;
exports.showCostModal     = showCostModal;


//----------------------------------------------------------------------------
// Small reusable view primitives: the Material 3 button and outlined field, and
// the Markdown rendering helpers (full and throttled) shared by the chat log.
//----------------------------------------------------------------------------

const ui  = require("./ui");
const md  = require("./markdown");
const i18n = require("./i18n");

const config = require("./config");
const color  = config.color;

const CLOSE_SVG = require("./icons").CLOSE_SVG;

//----------------------------------------------------------------------------

// Render Markdown + math into an assistant bubble (sanitized HTML). Empty text
// clears it.
function renderInto(bubble, text) {
    ui.setHTML(bubble, text ? md.render(text) : "");
};

// Throttled variant for streaming: re-parsing Markdown + KaTeX on every token
// is wasteful, so re-render at most ~10x/sec. finish() does a final full render
// so the last tokens always land.
function streamRender(bubble, text) {
    var now = Date.now();
    if (!bubble._mdAt || now - bubble._mdAt > 100) {
        bubble._mdAt = now;
        renderInto(bubble, text);
        return true;
    }
    return false;
};

//----------------------------------------------------------------------------

// M3 filled (primary) / outlined (secondary) button. labelLarge type: 14px/500.
function button(text, primary, events) {
    var styles = {
        display      : "inline-flex",
        alignItems   : "center",
        justifyContent : "center",
        padding      : "11px 20px",
        borderRadius : "10px",
        border       : primary ? "none" : "1px solid " + color.outlineVar,
        background   : primary ? color.primary : "transparent",
        color        : primary ? color.onPrimary : color.primary,
        font         : "inherit",
        fontSize     : "0.875rem",
        fontWeight   : "500",
        letterSpacing : "0.1px",
        cursor       : "pointer"
    };
    return ui.setText(ui.dom("button", styles, { type: "button" }, events), text);
};

// Outlined text field; the border tracks focus (outlineVariant -> primary), as
// OutlinedTextField does in the app.
function field(attributes) {
    var el = ui.dom("input", {
        width        : "100%",
        padding      : "14px",
        border       : "1px solid " + color.outlineVar,
        borderRadius : "10px",
        font         : "inherit",
        fontSize     : "1rem",
        color        : color.onSurface,
        background   : color.background,
        outline      : "none",
        boxSizing    : "border-box"
    }, attributes);
    el.addEventListener("focus", function () { el.style.borderColor = color.primary; });
    el.addEventListener("blur",  function () { el.style.borderColor = color.outlineVar; });
    return el;
};

//----------------------------------------------------------------------------

// A modal header row: the title on the leading edge, a close (X) icon on the
// trailing corner. The negative inline-end margin tucks the icon into the card's
// padding; logical properties keep it in the correct corner under RTL. Shared by
// every dialog so the close affordance reads identically and no dialog needs a
// bottom "Close" button (saves a row of height). `onDismiss` runs on click.
function modalHeader(titleText, onDismiss) {
    var header = ui.dom("div", {
        display: "flex", alignItems: "flex-start", justifyContent: "space-between", gap: "12px"
    });
    header.appendChild(ui.setText(ui.dom("div", {
        fontSize: "1.05rem", fontWeight: "600", color: color.onSurface, minWidth: "0"
    }), titleText));
    var closeBtn = ui.dom("button", {
        border: "none", background: "transparent", padding: "0",
        marginInlineEnd: "-6px", marginTop: "-2px",
        width: "32px", height: "32px", borderRadius: "16px", flexShrink: "0",
        display: "flex", alignItems: "center", justifyContent: "center",
        cursor: "pointer", color: color.onSurfaceVar
    }, { type: "button", title: i18n.t("close") });
    ui.setHTML(closeBtn, CLOSE_SVG.replace('width="12" height="12"', 'width="18" height="18"'));
    closeBtn.addEventListener("click", onDismiss);
    header.appendChild(closeBtn);
    return header;
};

//----------------------------------------------------------------------------

exports.renderInto   = renderInto;
exports.streamRender = streamRender;
exports.button       = button;
exports.field        = field;
exports.modalHeader  = modalHeader;

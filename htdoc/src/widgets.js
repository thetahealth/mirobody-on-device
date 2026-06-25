
//----------------------------------------------------------------------------
// Small reusable view primitives: the Material 3 button and outlined field, and
// the Markdown rendering helpers (full and throttled) shared by the chat log.
//----------------------------------------------------------------------------

const ui  = require("./ui");
const md  = require("./markdown");

const config = require("./config");
const color  = config.color;

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

exports.renderInto   = renderInto;
exports.streamRender = streamRender;
exports.button       = button;
exports.field        = field;

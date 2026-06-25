
//----------------------------------------------------------------------------
// The top app bar and its settings menu, styled like the app's
// CenterAlignedTopAppBar: brand on the left (which opens the history drawer when
// signed in), an optional centered element (the provider picker on chat), and
// the settings gear on the right.
//----------------------------------------------------------------------------

const ui   = require("./ui");
const net  = require("./net");
const i18n = require("./i18n");
const app  = require("./app");

const config = require("./config");
const color  = config.color;
const serifFamily = config.serifFamily;
const state  = config.state;
const LANGUAGE_KEY  = config.LANGUAGE_KEY;
const FONT_KEY      = config.FONT_KEY;
const languageLabel = config.languageLabel;
const fontTierLabel = config.fontTierLabel;
const applyDirection = config.applyDirection;
const applyFontScale = config.applyFontScale;

const SETTINGS_SVG = require("./icons").SETTINGS_SVG;

const modals = require("./modals");
const openHistory = require("./history").openHistory;

const logoUrl = require("./assets/mirobody.svg");

var t = i18n.t;

//----------------------------------------------------------------------------

// Top bar styled like the app's CenterAlignedTopAppBar: flat background, brand on
// the left, an optional centered element (the provider picker on chat), and
// optional right-side actions (the settings menu). centerEl/rightEl come from the
// active view via the topCenter/topRight slots.
function buildTopBar(centerEl, rightEl) {
    var bar = ui.dom("header", {
        display    : "flex",
        alignItems : "center",
        flex       : "0 0 auto",
        height     : "56px",
        padding    : "0 8px",
        background : color.background,
        borderBottom : "0.1px solid lightgray"
    });

    // Left zone (flex:1) and right zone (flex:1) are equal width so the center
    // element stays optically centered, as the app's centered title does.
    var left = ui.dom("div", {
        display: "flex", alignItems: "center", flex: "1 1 0", minWidth: "0"
    });
    // When signed in the logo (serif brand) opens the history drawer, as the app's
    // BrandLogo does. On the login screen the brand is omitted here -- the sign-in
    // view shows its own centered brand block (see login.js) -- so the bar carries
    // only the settings gear, matching the Theta Health login design.
    var signedIn = !!net.getToken();
    if (signedIn) {
        var brand = ui.dom("button", {
            display: "inline-flex", alignItems: "center", gap: "10px",
            textDecoration: "none", color: color.onSurface, padding: "8px",
            border: "none", background: "transparent", cursor: "pointer", font: "inherit"
        }, { type: "button", title: t("historyTitle") });
        brand.appendChild(ui.img(logoUrl, { width: "28px", height: "28px" }));
        // The title sits next to the logo but only on wider viewports; the
        // "brand-title" class is toggled by a min-width:600px media query (see
        // index.html) since inline styles can't express a breakpoint.
        brand.appendChild(ui.setText(ui.dom("span", {
            fontFamily: serifFamily, fontSize: "20px", fontWeight: "600",
            color: color.onSurface, whiteSpace: "nowrap"
        }, { "class": "brand-title" }), "Mirobody"));
        brand.addEventListener("click", openHistory);
        left.appendChild(brand);
    }

    var center = ui.dom("div", {
        display: "flex", alignItems: "center", justifyContent: "center",
        flex: "0 1 auto", minWidth: "0"
    });
    if (centerEl) { center.appendChild(centerEl); }

    var right = ui.dom("div", {
        display: "flex", alignItems: "center", justifyContent: "flex-end",
        flex: "1 1 0"
    });
    if (rightEl) { right.appendChild(rightEl); }

    bar.appendChild(left);
    bar.appendChild(center);
    bar.appendChild(right);
    return bar;
};

// Gear button + dropdown, mirroring the app's SettingsMenu: shows the signed-in
// email and a destructive "Sign out" item. Closes on an outside click.
function buildSettingsMenu() {
    var wrap = ui.dom("div", { position: "relative" });

    var gear = ui.dom("button", {
        border: "none", background: "transparent", padding: "0",
        width: "40px", height: "40px", borderRadius: "20px",
        display: "flex", alignItems: "center", justifyContent: "center",
        cursor: "pointer", color: color.onSurfaceVar
    }, { type: "button", title: t("settings") });
    ui.setHTML(gear, SETTINGS_SVG);

    var menu = ui.dom("div", {
        position: "absolute", top: "46px", insetInlineEnd: "4px",
        minWidth: "190px", background: "#ffffff",
        border: "1px solid " + color.outlineVar,
        borderRadius: "12px", boxShadow: "0 8px 24px rgba(0, 0, 0, 0.16)",
        padding: "6px", display: "none", flexDirection: "column", zIndex: "900"
    });

    function close() {
        menu.style.display = "none";
        document.removeEventListener("click", onDoc, true);
    };
    function onDoc(evt) {
        if (!wrap.contains(evt.target)) { close(); }
    };

    function divider() {
        return ui.dom("div", {
            height: "1px", background: color.outlineVar, opacity: "0.6", margin: "4px 0"
        });
    };

    // A menu row with a left label and an optional right-aligned hint (the
    // current value); `danger` paints it in the error color, as the app does for
    // sign-out. Returns the row and its hint node so callers can update the hint.
    function menuItem(label, hint, danger) {
        var row = ui.dom("button", {
            display: "flex", alignItems: "center", justifyContent: "space-between",
            gap: "16px", textAlign: "start", width: "100%", border: "none",
            background: "transparent", padding: "10px 12px", borderRadius: "8px",
            cursor: "pointer", font: "inherit", fontSize: "0.875rem",
            color: danger ? color.error : color.onSurface
        }, { type: "button" });
        row.appendChild(ui.setText(ui.dom("span", { whiteSpace: "nowrap" }), label));
        var hintEl = ui.setText(ui.dom("span", {
            fontSize: "0.8rem", color: color.onSurfaceVar, whiteSpace: "nowrap"
        }), hint || "");
        row.appendChild(hintEl);
        row.addEventListener("mouseenter", function () { row.style.background = color.surfaceLow; });
        row.addEventListener("mouseleave", function () { row.style.background = "transparent"; });
        return { el: row, hint: hintEl };
    };

    if (state.email) {
        menu.appendChild(ui.setText(ui.dom("div", {
            padding: "8px 12px", fontSize: "0.8rem", color: color.onSurfaceVar,
            overflow: "hidden", textOverflow: "ellipsis", whiteSpace: "nowrap"
        }), state.email));
        menu.appendChild(divider());
    }

    var lang = menuItem(t("language"), languageLabel(state.language), false);
    lang.el.addEventListener("click", function () {
        close();
        modals.showLanguageModal(state.language, function (code) {
            state.language = code;
            localStorage.setItem(LANGUAGE_KEY, code);
            i18n.setLang(code);
            applyDirection(code);   // flip <html dir> for Arabic / Hebrew
            // Re-render so every string picks up the new language. Skip while a
            // reply is streaming so we don't tear down the in-flight bubble; the
            // choice still applies to the next render and request.
            if (!state.streaming) {
                app.render();
            } else {
                ui.setText(lang.hint, languageLabel(code));
            }
        });
    });
    menu.appendChild(lang.el);

    var font = menuItem(t("fontSize"), t(fontTierLabel(state.fontOffset)), false);
    font.el.addEventListener("click", function () {
        close();
        modals.showFontSizeModal(state.fontOffset, function (offset) {
            state.fontOffset = offset;
            localStorage.setItem(FONT_KEY, String(offset));
            applyFontScale(offset);   // live: scales the whole UI immediately
            ui.setText(font.hint, t(fontTierLabel(offset)));
        });
    });
    menu.appendChild(font.el);

    var backend = menuItem(t("backend"), "", false);
    backend.el.addEventListener("click", function () {
        close();
        modals.showBackendModal();
    });
    menu.appendChild(backend.el);

    // Connect EHR joins the main items, but only when signed in (it needs a
    // session); About follows it and stays visible on the login screen too.
    if (net.getToken()) {
        var ehr = menuItem(t("ehrConnect"), "", false);
        ehr.el.addEventListener("click", function () { close(); require("./ehr").showEhrModal(); });
        menu.appendChild(ehr.el);

        menu.appendChild(divider());
    }

    var about = menuItem(t("about"), "", false);
    about.el.addEventListener("click", function () {
        close();
        modals.showAboutModal();
    });
    menu.appendChild(about.el);

    // Sign out only makes sense once authenticated; on the login screen the menu
    // is just language / font / backend / about.
    if (net.getToken()) {
        menu.appendChild(divider());

        var signOut2 = menuItem(t("signOut"), "", true);
        signOut2.el.addEventListener("click", function () { close(); app.signOut(); });
        menu.appendChild(signOut2.el);
    }

    gear.addEventListener("click", function (evt) {
        evt.stopPropagation();
        if (menu.style.display === "none") {
            menu.style.display = "flex";
            document.addEventListener("click", onDoc, true);
        } else {
            close();
        }
    });

    wrap.appendChild(gear);
    wrap.appendChild(menu);
    return wrap;
};

//----------------------------------------------------------------------------

exports.buildTopBar      = buildTopBar;
exports.buildSettingsMenu = buildSettingsMenu;

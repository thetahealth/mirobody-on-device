
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
const APP_VERSION    = config.APP_VERSION;

const SETTINGS_SVG = require("./icons").SETTINGS_SVG;
const PERSON_SVG   = require("./icons").PERSON_SVG;
const BACK_SVG     = require("./icons").BACK_SVG;

const modals = require("./modals");
const openHistory = require("./history").openHistory;

const logoUrl = require("./assets/mirobody.svg");

var t = i18n.t;

//----------------------------------------------------------------------------

// Top bar styled like the app's CenterAlignedTopAppBar: a left slot, the brand
// optically centered, and optional right-side actions (the settings menu).
// centerEl/rightEl come from the active view via the topCenter/topRight slots.
// leftMode picks the left affordance: "account" = the avatar that opens the
// drawer (chat); "cancelAdd" = a back arrow that cancels Add-account; "none" =
// empty (the plain login screen).
function buildTopBar(centerEl, rightEl, leftMode) {
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
    // "account": the top-left is an account avatar that opens the personal drawer
    // (history, health connections, account). A filled initial-avatar reads as
    // "you / account" -- distinct from the settings gear on the right (app
    // settings), so users aren't left guessing which menu is which. The initial
    // comes from the JWT email claim; with no email we fall back to a person glyph.
    var signedIn = leftMode === "account";
    if (signedIn) {
        var menuBtn = ui.dom("button", {
            border: "none", background: "transparent", padding: "0",
            width: "40px", height: "40px", borderRadius: "20px",
            display: "flex", alignItems: "center", justifyContent: "center",
            cursor: "pointer"
        }, { type: "button", title: t("menuTitle") });
        var avatar = ui.dom("div", {
            width: "30px", height: "30px", borderRadius: "50%",
            background: color.primary, color: color.onPrimary,
            display: "flex", alignItems: "center", justifyContent: "center",
            fontSize: "14px", fontWeight: "600", lineHeight: "1"
        });
        var email = net.getUserEmail();
        var initial = email ? email.charAt(0).toUpperCase() : "";
        if (initial) { ui.setText(avatar, initial); }
        else { ui.setHTML(avatar, PERSON_SVG); }
        menuBtn.appendChild(avatar);
        menuBtn.addEventListener("click", openHistory);
        left.appendChild(menuBtn);
    } else if (leftMode === "cancelAdd") {
        // Adding another account: a back arrow returns to the current account
        // without signing in.
        var backBtn = ui.dom("button", {
            border: "none", background: "transparent", padding: "0",
            width: "40px", height: "40px", borderRadius: "20px",
            display: "flex", alignItems: "center", justifyContent: "center",
            cursor: "pointer", color: color.onSurfaceVar
        }, { type: "button", title: t("back") });
        ui.setHTML(backBtn, BACK_SVG);
        backBtn.addEventListener("click", function () { app.cancelAddAccount(); });
        left.appendChild(backBtn);
    }

    var center = ui.dom("div", {
        display: "flex", alignItems: "center", justifyContent: "center",
        flex: "0 1 auto", minWidth: "0"
    });
    // Centered brand (logo + serif wordmark), the true CenterAlignedTopAppBar
    // title. It's static branding now -- the hamburger carries the nav action.
    // On narrow screens with shared subjects the subject picker needs the center
    // slot, so chat.js calls app.slots.setBrandVisible(false) to hand it over;
    // the two are mutually exclusive. On wide the subject sits in the composer,
    // so the center is free and the brand stays.
    if (signedIn) {
        var brand = ui.dom("div", {
            display: "inline-flex", alignItems: "center", gap: "8px", minWidth: "0"
        });
        brand.appendChild(ui.img(logoUrl, { width: "26px", height: "26px" }));
        var brandText = ui.setText(ui.dom("span", {
            fontFamily: serifFamily, fontSize: "20px", fontWeight: "600",
            color: color.onSurface, whiteSpace: "nowrap"
        }), "Mirobody");
        brand.appendChild(brandText);
        app.slots.setBrandVisible = function (show) {
            brand.style.display = show ? "inline-flex" : "none";
        };
        center.appendChild(brand);
    }
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
        minWidth: "190px", maxWidth: "300px", background: "#ffffff",
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
        row.appendChild(ui.setText(ui.dom("span", {
            whiteSpace: "nowrap", flex: "0 0 auto"
        }), label));
        // The hint takes the remaining width and ellipsizes -- the backend address
        // can be a long URL.
        var hintEl = ui.setText(ui.dom("span", {
            fontSize: "0.8rem", color: color.onSurfaceVar, whiteSpace: "nowrap",
            overflow: "hidden", textOverflow: "ellipsis", minWidth: "0",
            flex: "0 1 auto", textAlign: "end"
        }), hint || "");
        row.appendChild(hintEl);
        row.addEventListener("mouseenter", function () { row.style.background = color.surfaceLow; });
        row.addEventListener("mouseleave", function () { row.style.background = "transparent"; });
        return { el: row, hint: hintEl };
    };

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

    var backend = menuItem(t("backend"), net.getBaseUrl(), false);
    backend.el.addEventListener("click", function () {
        close();
        modals.showBackendModal();
    });
    menu.appendChild(backend.el);

    // About: opens a small dialog with the build version. The gear menu is
    // intentionally just the app settings (language / font / backend / about) so
    // it's identical on the login and chat screens. Everything session-scoped --
    // health connections (care circle / EHR / devices), history, and Sign out --
    // lives in the left nav drawer instead (chat only; see history.js).
    var about = menuItem(t("about"), APP_VERSION ? ("v" + APP_VERSION) : "", false);
    about.el.addEventListener("click", function () {
        close();
        modals.showAlertModal(t("about"), t("version", APP_VERSION), t("close"));
    });
    menu.appendChild(about.el);

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

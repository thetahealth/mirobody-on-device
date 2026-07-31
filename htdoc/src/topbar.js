
//----------------------------------------------------------------------------
// The top app bar, styled like the app's CenterAlignedTopAppBar: a hamburger on the
// left that opens the nav drawer, the "Mirobody" wordmark beside it on chat whenever
// the center is free, and an optional centered element (the subject picker on chat).
// There are no right-hand actions -- the settings gear that used to sit there now
// lives inside the drawer (see history.js), so every screen, login included, has
// exactly one menu affordance.
//----------------------------------------------------------------------------

const ui   = require("./ui");
const i18n = require("./i18n");
const app  = require("./app");

const config = require("./config");
const color  = config.color;
const serifFamily = config.serifFamily;

const MENU_SVG = require("./icons").MENU_SVG;
const BACK_SVG = require("./icons").BACK_SVG;

const openHistory = require("./history").openHistory;

var t = i18n.t;

//----------------------------------------------------------------------------

// Top bar styled like the app's CenterAlignedTopAppBar: a left slot and an optically
// centered middle. centerEl comes from the active view via the topCenter slot.
// leftMode names the screen, which is what the left zone varies by:
//   "chat"      -- signed in: hamburger, plus the wordmark when the center is free.
//   "login"     -- the login view: hamburger only. Its card already carries the logo
//                  and a display-size "Mirobody", so a second one in the bar would
//                  just repeat the brand twice on one screen.
//   "cancelAdd" -- the login view opened to add another account: a back arrow to the
//                  current account instead (a drawer would be a dead end), and no
//                  wordmark, for the same reason as "login".
function buildTopBar(centerEl, leftMode) {
    var bar = ui.dom("header", {
        display    : "flex",
        alignItems : "center",
        flex       : "0 0 auto",
        height     : "56px",
        padding    : "0 8px",
        background : color.background,
        borderBottom : "0.1px solid " + color.outlineVar
    });

    // Left zone (flex:1) and right zone (flex:1) are equal width so the center
    // element stays optically centered, as the app's centered title does.
    var left = ui.dom("div", {
        display: "flex", alignItems: "center", flex: "1 1 0", minWidth: "0"
    });
    // The top-left hamburger opens the nav drawer -- history, health connections, app
    // settings, account. A nav glyph rather than the person glyph this used to be: the
    // drawer's primary content is the conversation list (its only scrolling band) and
    // account is one pinned row at the bottom, so a profile icon undersold what the
    // button opens. It shows on login too, where the drawer narrows to the app-settings
    // group -- that is how that screen reaches language / font / appearance / backend
    // now that the gear is gone.
    if (leftMode === "chat" || leftMode === "login") {
        var menuBtn = ui.dom("button", {
            border: "none", background: "transparent", padding: "0",
            width: "40px", height: "40px", borderRadius: "20px",
            display: "flex", alignItems: "center", justifyContent: "center",
            cursor: "pointer", color: color.onSurfaceVar
        }, { type: "button", title: t("menuTitle") });
        ui.setHTML(menuBtn, MENU_SVG);
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

    // Wordmark, next to the hamburger on chat: without it the bar is a blank 56px
    // strip, since the center slot is empty on a wide screen and often empty on a
    // narrow one too. It shares that space with chat's subject picker (mobile puts the
    // picker in the center), so the two are alternatives -- but the picker's visibility
    // isn't known until /api/circle/health-shared-with-me answers, well after this bar
    // is built. A build-time `if (!centerEl)` therefore got it wrong on mobile: the
    // slot holds the picker element from the start, yet that element stays
    // display:none unless a member actually shared health data, leaving a blank bar for
    // everyone else. So the wordmark starts shown and chat's toggleSubject hides it if
    // and when the picker takes over -- see the slots.wordmark handoff in app.js.
    // Same serif ink as the login card's brand title, at bar scale; the login screens
    // are excluded, since that card is already the brand statement.
    if (leftMode === "chat") {
        var wordmark = ui.setText(ui.dom("div", {
            fontFamily: serifFamily, fontWeight: "600", fontSize: "1.15rem",
            letterSpacing: "-0.2px", color: color.wordmark,
            marginInlineStart: "4px", whiteSpace: "nowrap",
            overflow: "hidden", textOverflow: "ellipsis", minWidth: "0"
        }), "Mirobody");
        left.appendChild(wordmark);
        app.slots.wordmark = wordmark;
    }

    var center = ui.dom("div", {
        display: "flex", alignItems: "center", justifyContent: "center",
        flex: "0 1 auto", minWidth: "0"
    });
    // The center hosts only what the active view puts there (chat's subject picker on
    // a narrow screen); when it's empty the wordmark above takes the bar instead. It
    // used to be the wordmark's own home, centered, but then the picker and the brand
    // were competing for one slot -- keeping them in separate zones lets either appear
    // without displacing the other.
    if (centerEl) { center.appendChild(centerEl); }

    // An empty trailing zone, the same flex width as the left one: it carries no
    // actions now, but the matched pair is what keeps the center element optically
    // centered rather than shoved right by the hamburger.
    var right = ui.dom("div", { flex: "1 1 0" });

    bar.appendChild(left);
    bar.appendChild(center);
    bar.appendChild(right);
    return bar;
};

//----------------------------------------------------------------------------

exports.buildTopBar = buildTopBar;

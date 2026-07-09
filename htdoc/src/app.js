
//----------------------------------------------------------------------------
// The orchestration hub: the top-level render() that swaps between the login and
// chat views, signOut(), and the page-load provider fetch -- plus the `slots`
// object the views fill (top-bar center/right elements) and read back. Every
// view requires this module and calls app.render() / app.signOut(), so it is the
// one place the otherwise-circular view graph converges. The view modules are
// required lazily inside render() (not at load time) to keep that graph acyclic.
//----------------------------------------------------------------------------

const ui  = require("./ui");
const net = require("./net");
const db  = require("./db");

const config = require("./config");
const state  = config.state;

// Top-bar slots for the current view, filled by buildChat/buildLogin and read by
// render() to compose the bar -- mirrors the app's CenterAlignedTopAppBar, which
// hosts the provider picker (center) and the settings menu (right). providerRefill
// is set by buildChat to the active selector's refill function, so the page-load
// /api/providers fetch can populate it once the data arrives.
var slots = {
    topCenter     : null,
    topRight      : null,
    providerRefill : null
};

// True while "Add account" is showing the login view over an existing session
// (a token is still stored, but we render login so a second account can sign in).
// In-memory only: a reload just returns to the current account.
var addingAccount = false;

//----------------------------------------------------------------------------

function render() {
    var appEl = ui.clear(document.getElementById("app"));
    ui.setStyle(appEl, {
        display       : "flex",
        flexDirection : "column",
        height        : "100vh"
    });
    // Prefer the dynamic viewport height where supported: on mobile, 100vh spans
    // the area *behind* the soft keyboard, so the bottom composer ends up hidden
    // under it; 100dvh tracks the visible viewport and keeps the composer above
    // the keyboard. An unsupported value is ignored, leaving the 100vh fallback.
    appEl.style.height = "100dvh";

    // The views (and the top bar they pull in) are required here, at call time,
    // rather than at module load -- by now this module's exports are complete, so
    // their `require("./app")` resolves to a fully-populated object.
    var buildChat    = require("./chat").buildChat;
    var buildLogin   = require("./login").buildLogin;
    var buildTopBar  = require("./topbar").buildTopBar;
    var consent      = require("./consent");

    // Build the body first so it fills the topCenter/topRight slots, then the
    // bar, then mount bar-above-body.
    slots.topCenter = null;
    slots.topRight  = null;
    // An OAuth consent request (?oauth_consent=...) takes over the body once the
    // user is signed in; until then the normal login view runs and we resume
    // here automatically after completeLogin re-renders.
    var token = net.getToken();
    // "Add account" forces the login view even though a token is still stored.
    var showLogin = addingAccount || !token;
    var body;
    if (consent.active() && token && !addingAccount) {
        body = consent.build();
    } else {
        body = showLogin ? buildLogin() : buildChat();
    }
    // Top-bar left: the account avatar in chat; a back arrow to cancel while
    // adding an account; nothing on the plain login screen.
    var leftMode = addingAccount ? "cancelAdd" : (token ? "account" : "none");
    appEl.appendChild(buildTopBar(slots.topCenter, slots.topRight, leftMode));
    appEl.appendChild(body);
};

//----------------------------------------------------------------------------

// Sign out the CURRENT account. Drops its local chat mirror (before clearToken,
// which needs the still-current token to derive the key), removes its token, then
// falls back to another stored account when one exists -- else lands on login.
// activateSession() handles both: with no token left, it renders the login view.
function signOut() {
    db.clear();
    net.clearToken();
    // Modals/history mount their backdrop on document.body as siblings of #app,
    // so render() (which only clears #app) wouldn't remove them -- tear down any
    // open overlay here so a 401 mid-modal doesn't leave it floating over login.
    closeOverlays();
    activateSession();
};

// Reset per-session state and load the CURRENT account's data, then render. The
// single entry point after any session change (fresh login, account switch, or
// signing out the current account), so no previous account's state leaks through.
function activateSession() {
    addingAccount = false;
    state.email     = "";
    state.messages  = [];
    state.providers = [];
    state.streaming = false;
    state.currentConversationId = "";
    state.readOnly  = false;
    state.currentSubjectId = "";
    state.incognito = false;
    state.incognitoSaved = null;
    // Provider list is auth-gated; loadProviders no-ops when signed out.
    loadProviders();
    db.loadMessages().then(function (msgs) {
        if (msgs && msgs.length) { state.messages = msgs; }
    }).catch(function () {}).then(function () {
        render();
        // A care-circle invite link consumed before sign-in: accept it now that a
        // session exists (no-op when nothing is stashed / signed out).
        require("./circle_accept").consume();
    });
};

// Switch to another already-stored account (from the drawer's account switcher).
function switchAccount(sub) {
    if (net.switchAccount(sub)) {
        closeOverlays();   // dismiss the drawer
        activateSession();
    }
};

// Show the login view over the current session so a second account can sign in;
// the existing account's token stays stored (see net.setToken).
function addAccount() {
    addingAccount = true;
    closeOverlays();
    render();
};

// Back out of "Add account" without signing in, returning to the current account.
function cancelAddAccount() {
    addingAccount = false;
    render();
};

// Remove every body-level overlay (any element that isn't the #app root). Used
// by signOut so resetting to the login view also dismisses open modals.
function closeOverlays() {
    if (typeof document === "undefined" || !document.body) { return; }
    var kids = document.body.children;
    for (var i = kids.length - 1; i >= 0; i --) {
        if (kids[i].id !== "app") { document.body.removeChild(kids[i]); }
    }
};

//----------------------------------------------------------------------------

// Finish a successful sign-in: store the token, then restore THIS user's locally
// persisted conversation (keyed per user in IndexedDB) before rendering, so a
// returning user sees their own history and never the previous account's. Called
// by every sign-in success path (email, Google, Apple, WeChat) in place of a
// bare setToken + render.
function completeLogin(accessToken) {
    net.setToken(accessToken);   // stores under this account's slot + makes current
    activateSession();
};

//----------------------------------------------------------------------------

// POST /api/providers and cache the list in state, refilling the selector if the
// chat view is already mounted (if the response arrives first, buildChat fills
// from state.providers instead).
//
// The endpoint is JWT-guarded (it exposes the configured agent/provider names),
// so this is a no-op until the user is signed in: it runs on a page load that
// already has a token and is kicked off by completeLogin after a fresh sign-in,
// never from the login screen.
function loadProviders() {
    if (!net.getToken()) { return; }
    net.post(
        "/api/providers",
        null,
        function (data) { // onsuccess
            state.providers = (data instanceof Array) ? data : [];
            if (slots.providerRefill instanceof Function) {
                slots.providerRefill(state.providers);
            }
        },
        function () { /* leave the default option in place (a 401 signs out centrally) */ }
    );
};

//----------------------------------------------------------------------------

// Route every 401 (any request, including the chat stream) through signOut so a
// rejected/expired token always falls back to the login view -- see net.post/get/stream.
net.setUnauthorizedHandler(signOut);

exports.slots            = slots;
exports.render           = render;
exports.signOut          = signOut;
exports.completeLogin    = completeLogin;
exports.loadProviders    = loadProviders;
exports.switchAccount    = switchAccount;
exports.addAccount       = addAccount;
exports.cancelAddAccount = cancelAddAccount;

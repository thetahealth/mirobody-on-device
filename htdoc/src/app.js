
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

//----------------------------------------------------------------------------

function render() {
    var appEl = ui.clear(document.getElementById("app"));
    ui.setStyle(appEl, {
        display       : "flex",
        flexDirection : "column",
        height        : "100vh"
    });

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
    var body;
    if (consent.active() && token) {
        body = consent.build();
    } else {
        body = token ? buildChat() : buildLogin();
    }
    appEl.appendChild(buildTopBar(slots.topCenter, slots.topRight));
    appEl.appendChild(body);
};

//----------------------------------------------------------------------------

function signOut() {
    db.clear();   // drop this user's local chat history (keyed by the still-present token)
    net.clearToken();
    state.email     = "";
    state.messages  = [];
    state.providers = [];   // don't leak the previous session's provider list
    state.streaming = false;
    // Modals/history mount their backdrop on document.body as siblings of #app,
    // so render() (which only clears #app) wouldn't remove them -- tear down any
    // open overlay here so a 401 mid-modal doesn't leave it floating over login.
    closeOverlays();
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
    net.setToken(accessToken);
    state.messages = [];
    // The provider list is auth-gated, so it isn't fetched on the login screen;
    // load it now that we hold a token.
    loadProviders();
    db.loadMessages().then(function (msgs) {
        if (msgs && msgs.length) { state.messages = msgs; }
    }).catch(function () {}).then(function () {
        render();
    });
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

exports.slots         = slots;
exports.render        = render;
exports.signOut       = signOut;
exports.completeLogin = completeLogin;
exports.loadProviders = loadProviders;

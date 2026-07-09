require("./index.css");

// On-device debug console (vConsole) for mobile browsers where there are no
// devtools. Off by default; enable by opening the app with ?debug (or ?vconsole)
// and disable with ?debug=0 (or ?vconsole=0). The bare flag counts as on. The
// choice is persisted in localStorage so it survives reloads and the URL-cleaning
// the OAuth/invite consumers do below. Loaded via dynamic import() so webpack
// code-splits it into its own chunk -- the normal bundle is unaffected when
// debugging is off. Safe under the page CSP: the chunk is same-origin (script-src
// 'self') and vConsole's injected <style> is permitted by style-src
// 'unsafe-inline'. Runs first so it captures startup logs.
(function () {
    try {
        var q = new URLSearchParams(window.location.search);
        // Tri-state per name: absent -> null; present but "0"/"false"/"off" ->
        // false (turn off); present otherwise (bare flag or any other value) -> true.
        var read = function (name) {
            if (!q.has(name)) { return null; }
            var v = q.get(name);
            return !(v === "0" || v === "false" || v === "off");
        };
        var want = read("debug");
        if (want === null) { want = read("vconsole"); }
        if (want === true)  { localStorage.setItem("mb_debug", "1"); }
        if (want === false) { localStorage.removeItem("mb_debug"); }
        if (localStorage.getItem("mb_debug") === "1") {
            import(/* webpackChunkName: "vconsole" */ "vconsole")
                .then(function (m) { new (m.default || m)(); })
                .catch(function () {});
        }
    } catch (e) { /* private-mode localStorage, etc. -- debugging is optional */ }
})();

const i18n = require("./i18n");
const db   = require("./db");

const config = require("./config");
const state  = config.state;

const app  = require("./app");
const auth = require("./auth");

//----------------------------------------------------------------------------
// Entry point: apply the saved language / font scale / text direction, restore
// any locally-persisted conversation, then render and fetch the provider list.
// All of the view code lives in the modules required by ./app (chat, login,
// topbar, modals, history, auth) -- this file just wires up startup.
//----------------------------------------------------------------------------

// Localize the UI, apply the saved font scale, and set the text direction
// before the first render.
i18n.setLang(state.language);
config.applyFontScale(state.fontOffset);
config.applyDirection(state.language);

// A care-circle invite link (?circle_token=...): stash the token and clean the
// URL now, before any render. It's accepted once we hold a session -- below if
// already signed in, otherwise after sign-in (see app.completeLogin).
require("./circle_accept").capture();

// If we arrived back from a WeChat or GitHub sign-in redirect
// (?code=...&state=...), exchange the code for tokens. Each consumer handles the
// callback only if the returned state matches the nonce it issued, so at most one
// runs; it strips the query from the URL and triggers its own re-render on
// completion (chat on success, the login view with an error on failure).
var handlingRedirect = auth.consumeWeChatRedirect() || auth.consumeGitHubRedirect();

// Returning from an EHR (SMART on FHIR) connect? The server already did the token
// exchange and bounced us back with ?ehr=<status>; surface it (and offer to sync).
// Not a token-exchange redirect, so it doesn't suppress the render below.
require("./ehr").consumeEhrRedirect();

// Same for a vendor OAuth connect redirect (?vendor=connected|expired|error).
require("./vendors").consumeVendorRedirect();

// Restore any locally-persisted conversation before the first paint so the chat
// panel comes up with prior messages (and scrolled to the latest), plus its server
// thread id so the next turn continues that same thread rather than forking a new
// one. Best-effort: if IndexedDB is unavailable we just render an empty conversation.
Promise.all([db.loadMessages(), db.loadConversationId()]).then(function (r) {
    var msgs = r[0], convId = r[1];
    if (msgs && msgs.length) { state.messages = msgs; }
    if (convId) { state.currentConversationId = convId; }
}).catch(function () {}).then(function () {
    // Skip the initial render while an OAuth code exchange is in flight: the
    // consumer re-renders once it resolves. This avoids briefly mounting the
    // login view for a sign-in that's already underway -- which would fire its
    // provider-config probes (GET /{google,apple,wechat,github}/verify) need-
    // lessly, right as the sign-in completes.
    if (!handlingRedirect) { app.render(); }
    // Already signed in with a stashed invite token? Accept it now.
    require("./circle_accept").consume();
});
app.loadProviders();

// Re-render when the mobile/desktop breakpoint is crossed so the layout tracks
// the viewport. Fires only on a crossing (not every resize); skipped mid-stream
// so an in-flight reply isn't torn down.
if (window.matchMedia) {
    var mq = window.matchMedia(config.MOBILE_QUERY);
    var onBreakpoint = function () { if (!state.streaming) { app.render(); } };
    if (mq.addEventListener) {
        mq.addEventListener("change", onBreakpoint);
    } else if (mq.addListener) {
        mq.addListener(onBreakpoint); // older browsers
    }
}

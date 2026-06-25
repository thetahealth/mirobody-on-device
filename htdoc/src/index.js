require("./index.css");

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

// Restore any locally-persisted conversation before the first paint so the chat
// panel comes up with prior messages (and scrolled to the latest). Best-effort:
// if IndexedDB is unavailable we just render an empty conversation.
db.loadMessages().then(function (msgs) {
    if (msgs && msgs.length) { state.messages = msgs; }
}).catch(function () {}).then(function () {
    // Skip the initial render while an OAuth code exchange is in flight: the
    // consumer re-renders once it resolves. This avoids briefly mounting the
    // login view for a sign-in that's already underway -- which would fire its
    // provider-config probes (GET /{google,apple,wechat,github}/verify) need-
    // lessly, right as the sign-in completes.
    if (!handlingRedirect) { app.render(); }
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

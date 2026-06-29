
//----------------------------------------------------------------------------
// Care-circle invite link (?circle_token=...). On load we stash the token and
// strip it from the URL (capture); once the user is signed in we POST it to
// /api/circle/accept (consume). Split so the token survives the sign-in step
// when the invitee isn't logged in yet -- the same shape as the OAuth redirect
// consumers, but persisted because accepting needs a session.
//----------------------------------------------------------------------------

const net  = require("./net");
const i18n = require("./i18n");

var t = i18n.t;
var KEY = "mb-circle-token";

// Pull ?circle_token= out of the URL into sessionStorage and clean the address
// bar (so a reload / sign-in doesn't re-trigger or leak it). No-op when absent.
function capture() {
    try {
        var params = new URLSearchParams(window.location.search);
        var tok = params.get("circle_token");
        if (!tok) { return; }
        sessionStorage.setItem(KEY, tok);
        params.delete("circle_token");
        var qs  = params.toString();
        var url = window.location.pathname + (qs ? "?" + qs : "") + window.location.hash;
        window.history.replaceState({}, document.title, url);
    } catch (e) {}
};

// If signed in and a token is stashed, accept the invite (once) and report the
// result. Called on startup and again after a fresh sign-in.
function consume() {
    if (!net.getToken()) { return; }
    var tok = "";
    try { tok = sessionStorage.getItem(KEY) || ""; } catch (e) {}
    if (!tok) { return; }
    try { sessionStorage.removeItem(KEY); } catch (e) {}

    net.post("/api/circle/accept", { token: tok }, function () {
        require("./modals").showConfirmModal(t("careCircle"), t("circleAcceptOk"), t("close"), false, function () {});
    }, function () {
        require("./modals").showConfirmModal(t("careCircle"), t("circleAcceptFail"), t("close"), false, function () {});
    });
};

exports.capture = capture;
exports.consume = consume;

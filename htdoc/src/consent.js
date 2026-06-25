
//----------------------------------------------------------------------------
// OAuth 2.0 consent view. The authorization server (oauth::OAuthService) sends
// the browser here with ?oauth_consent=<handle> after validating an MCP/OAuth
// client's authorize request. We reuse the app's existing sign-in: when no token
// is held, app.render() shows the normal login view and resumes here once the
// user is authenticated. With a token we fetch the request details
// (GET /oauth/authorize/info) to show what the client is asking for, then POST
// the user's decision (POST /oauth/authorize/decision); the server returns the
// final redirect back to the client (with ?code=... on approve, ?error=... on
// deny), which we navigate to.
//----------------------------------------------------------------------------

const net = require("./net");
const app = require("./app");
const ui  = require("./ui");

var CONSENT_PARAM = "oauth_consent";

var info          = null;    // {client_name, redirect_uri, scopes} once loaded
var infoRequested = false;
var loadError     = "";      // set when the handle is expired/invalid
var submitting    = false;   // disables the buttons during the decision POST

// Human-readable blurb per known scope; unknown scopes show verbatim.
var SCOPE_LABELS = {
    "mcp:read"  : "Read your data through the MCP tools",
    "mcp:write" : "Take actions on your behalf through the MCP tools"
};

// The pending-request handle from the URL, or "" when this is not a consent load.
function handle() {
    var params = new URLSearchParams(window.location.search || "");
    return params.get(CONSENT_PARAM) || "";
}

// True when the current page load is an OAuth consent request.
function active() {
    return !!handle();
}

// Fetch the request details once; re-render when they arrive so build() can
// swap the placeholder for the real card.
function loadInfo() {
    if (infoRequested) { return; }
    infoRequested = true;
    net.get(
        "/oauth/authorize/info?req=" + encodeURIComponent(handle()),
        function (data) { info = (data && typeof data === "object") ? data : {}; app.render(); },
        function (msg)  { loadError = msg || "This authorization request has expired."; app.render(); }
    );
}

// Leave consent mode by stripping the query and returning to the app shell. Used
// only for the local "expired -> go back" affordance; the approve/deny paths
// navigate to the client's redirect URI instead.
function dismiss() {
    if (window.history && window.history.replaceState) {
        window.history.replaceState(null, "", window.location.origin + window.location.pathname);
    }
    app.render();
}

// POST the user's decision; on success navigate to the client's redirect URI.
function decide(approve) {
    if (submitting) { return; }
    submitting = true;
    app.render();
    net.post(
        "/oauth/authorize/decision",
        { req: handle(), approve: approve },
        function (data) {
            if (data && data.redirect) {
                window.location.href = data.redirect;   // back to the OAuth client
            } else {
                submitting = false;
                loadError = "Something went wrong completing the authorization.";
                app.render();
            }
        },
        function (msg) {
            submitting = false;
            loadError = msg || "Something went wrong completing the authorization.";
            app.render();
        }
    );
}

//----------------------------------------------------------------------------

// Build the consent view element. Called by app.render() only when active() and
// a token is held (the login view is shown first otherwise).
function build() {
    if (!info && !loadError) { loadInfo(); }

    var wrap = ui.dom("div", {
        flex           : "1 1 auto",
        display        : "flex",
        alignItems     : "center",
        justifyContent : "center",
        padding        : "24px",
        overflow       : "auto"
    });

    var card = ui.dom("div", {
        maxWidth     : "420px",
        width        : "100%",
        padding      : "24px",
        borderRadius : "12px",
        border       : "1px solid rgba(128,128,128,0.3)",
        boxShadow    : "0 2px 12px rgba(0,0,0,0.08)"
    });
    wrap.appendChild(card);

    if (loadError) {
        card.appendChild(ui.setText(ui.dom("h2", { margin: "0 0 12px" }), "Authorization unavailable"));
        card.appendChild(ui.setText(ui.dom("p", { margin: "0 0 20px", opacity: "0.8" }), loadError));
        card.appendChild(ui.setText(
            ui.dom("button", buttonStyle(false), null, { click: dismiss }), "Back"));
        return wrap;
    }

    if (!info) {
        card.appendChild(ui.setText(ui.dom("p", { opacity: "0.7" }), "Loading authorization request..."));
        return wrap;
    }

    var clientName = info.client_name || "An application";

    card.appendChild(ui.setText(ui.dom("h2", { margin: "0 0 8px" }), "Authorize access"));
    card.appendChild(ui.setText(
        ui.dom("p", { margin: "0 0 16px", lineHeight: "1.5" }),
        clientName + " wants to connect to your mirobody account."));

    var scopes = (info.scopes instanceof Array) ? info.scopes : [];
    if (scopes.length) {
        card.appendChild(ui.setText(
            ui.dom("div", { fontWeight: "600", margin: "0 0 6px" }), "It will be able to:"));
        var list = ui.dom("ul", { margin: "0 0 20px", paddingLeft: "20px", lineHeight: "1.6" });
        for (var i = 0; i < scopes.length; i++) {
            list.appendChild(ui.setText(ui.dom("li"), SCOPE_LABELS[scopes[i]] || scopes[i]));
        }
        card.appendChild(list);
    }

    var row = ui.dom("div", { display: "flex", gap: "12px", marginTop: "8px" });
    var deny = ui.setText(
        ui.dom("button", buttonStyle(false), submitting ? { disabled: "true" } : null,
               { click: function () { decide(false); } }), "Deny");
    var allow = ui.setText(
        ui.dom("button", buttonStyle(true), submitting ? { disabled: "true" } : null,
               { click: function () { decide(true); } }), submitting ? "Authorizing..." : "Allow");
    row.appendChild(deny);
    row.appendChild(allow);
    card.appendChild(row);

    return wrap;
}

function buttonStyle(primary) {
    return {
        flex         : "1 1 0",
        padding      : "10px 16px",
        borderRadius : "8px",
        border       : primary ? "none" : "1px solid rgba(128,128,128,0.4)",
        background   : primary ? "#2563eb" : "transparent",
        color        : primary ? "#fff" : "inherit",
        fontSize     : "15px",
        cursor       : "pointer"
    };
}

//----------------------------------------------------------------------------

exports.active = active;
exports.build  = build;

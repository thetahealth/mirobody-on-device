
//----------------------------------------------------------------------------

// The client is served same-origin from res/htdoc, so request URIs are
// relative. In webpack dev mode the dev server proxies them to the C++ backend.

// Per-account token storage: each account's JWT lives under TOKEN_PREFIX+<sub>,
// and CURRENT_KEY names the active sub. Several accounts can coexist in one
// browser (quick-switch from the drawer) instead of one token clobbering the
// next. LEGACY_KEY is the old single-token slot, migrated once on load.
var TOKEN_PREFIX = "mirobody-x-token-";
var CURRENT_KEY  = "mirobody-x-current";
var LEGACY_KEY   = "mirobody-x-token";

// Decode a JWT's payload (middle segment) into an object, or null when the token
// is missing/malformed. Base64url -> UTF-8 JSON; escape()+decodeURIComponent
// round-trips UTF-8 byte values from atob.
function decodePayload(token) {
    if (!token) { return null; }
    var parts = token.split(".");
    if (parts.length < 2) { return null; }
    try {
        var b64 = parts[1].replace(/-/g, "+").replace(/_/g, "/");
        while (b64.length % 4) { b64 += "="; }
        return JSON.parse(decodeURIComponent(escape(atob(b64))));
    } catch (e) {
        return null;
    }
};

function currentSub() { return localStorage.getItem(CURRENT_KEY) || ""; }

function getToken() {
    var sub = currentSub();
    return sub ? (localStorage.getItem(TOKEN_PREFIX + sub) || "") : "";
};

// Store a token under its own account slot and make it current. Dedupes by
// email: the server re-salts `sub` per issuance (jwt subject_salt), so the same
// account re-logging in gets a new sub -- drop any existing slot with the same
// email first so there's one entry per account, not one per login.
function setToken(token) {
    if (!token) { return; }
    var p = decodePayload(token);
    var sub = (p && p.sub) ? String(p.sub) : "";
    if (!sub) { return; }
    var email = (p && p.email) ? String(p.email) : "";
    if (email) {
        listAccounts().forEach(function (a) {
            if (a.sub !== sub && a.email === email) {
                localStorage.removeItem(TOKEN_PREFIX + a.sub);
            }
        });
    }
    localStorage.setItem(TOKEN_PREFIX + sub, token);
    localStorage.setItem(CURRENT_KEY, sub);
};

// Sign out the CURRENT account: drop its slot, then fall back to another stored
// account if one exists (returns true, now current) or clear the pointer
// (returns false -> the caller shows the login screen).
function clearToken() {
    var sub = currentSub();
    if (sub) { localStorage.removeItem(TOKEN_PREFIX + sub); }
    var rest = listAccounts();
    if (rest.length) {
        localStorage.setItem(CURRENT_KEY, rest[0].sub);
        return true;
    }
    localStorage.removeItem(CURRENT_KEY);
    return false;
};

// Make an already-stored account current. Returns false if its slot is gone.
function switchAccount(sub) {
    if (sub && localStorage.getItem(TOKEN_PREFIX + sub)) {
        localStorage.setItem(CURRENT_KEY, sub);
        return true;
    }
    return false;
};

// Every stored account as { sub, email, current }, the current one first.
function listAccounts() {
    var out = [];
    var cur = currentSub();
    for (var i = 0; i < localStorage.length; i++) {
        var k = localStorage.key(i);
        if (!k || k.indexOf(TOKEN_PREFIX) !== 0) { continue; }
        var sub = k.slice(TOKEN_PREFIX.length);
        var p = decodePayload(localStorage.getItem(k));
        out.push({ sub: sub, email: (p && p.email) ? String(p.email) : "", current: sub === cur });
    }
    out.sort(function (a, b) { return (b.current ? 1 : 0) - (a.current ? 1 : 0); });
    return out;
};

// One-time migration of the old single-token slot into the per-account scheme.
function migrateLegacy() {
    var legacy = localStorage.getItem(LEGACY_KEY);
    if (legacy) {
        setToken(legacy);
        localStorage.removeItem(LEGACY_KEY);
    }
};
migrateLegacy();

function decodeToken() { return decodePayload(getToken()); }

// A stable-per-session identifier for the signed-in user (the token's `sub`),
// used to namespace per-user local data so accounts never share it. "" = nobody.
function getUserId() {
    var p = decodeToken();
    return p ? String(p.sub || p.email || "") : "";
};

// The signed-in user's email, from the token's `email` claim (the mb_oauth access
// token carries it; see oauth/service.cpp). "" when absent -- e.g. a social login
// whose provider returned no email. Reliable across reloads, unlike the in-memory
// state.email, so it's the source for display and the account switcher.
function getUserEmail() {
    var p = decodeToken();
    return (p && p.email) ? String(p.email) : "";
};

exports.getToken      = getToken;
exports.setToken      = setToken;
exports.clearToken    = clearToken;
exports.switchAccount = switchAccount;
exports.listAccounts  = listAccounts;
exports.getUserId     = getUserId;
exports.getUserEmail  = getUserEmail;

//----------------------------------------------------------------------------

// Single reaction point for "token rejected" (any HTTP 401, including a 401 on
// the SSE stream). app.js registers signOut here, so every request bounces to
// the login view from one place instead of each call site re-checking status.
// When set, a 401 invokes it and the request's own onerror is skipped (the view
// is being torn down anyway).
var onUnauthorized = null;
function unauthorized() {
    if (onUnauthorized instanceof Function) { onUnauthorized(); return true; }
    return false;
};
exports.setUnauthorizedHandler = function (fn) { onUnauthorized = fn; };

//----------------------------------------------------------------------------

// Backend base URL prefixed to every request URI. Defaults to appBase() -- the
// "schema://host/prefix" the app is served from -- so requests target the same
// (possibly sub-path-mounted) server that served the client. A value set via the
// Backend setting overrides it, e.g. to point at a remote backend; that backend
// must then allow this origin via CORS for the cross-origin POST/stream to work.
var BASE_URL_KEY = "mirobody-base-url";

// The origin + URI prefix this client is mounted under (e.g.
// "http://host/mirobody"), derived from the page's own URL. The server mounts
// the whole app under HTTP_URI_PREFIX and redirects "/prefix" -> "/prefix/", so
// the page always loads at the mount root and its directory IS the prefix. We
// derive it here in the bundle rather than via an inline <script> in index.html
// because the page's CSP (script-src 'self') blocks inline scripts. Returns the
// bare origin when served from the root.
function appBase() {
    if (typeof window === "undefined") return "";
    // Directory of the current document: drop the last path segment (a trailing
    // file like "index.html", or nothing after the mount's trailing slash).
    var dir = window.location.pathname.replace(/[^\/]*$/, "");
    return (window.location.origin + dir).replace(/\/+$/, "");
};
function getBaseUrl() {
    return localStorage.getItem(BASE_URL_KEY) || appBase();
};
function setBaseUrl(url) {
    if (url) {
        localStorage.setItem(BASE_URL_KEY, url);
    } else {
        localStorage.removeItem(BASE_URL_KEY);
    }
};
function apiUrl(uri) {
    var base = getBaseUrl();
    return base ? base + uri : uri;
};

exports.appBase    = appBase;
exports.getBaseUrl = getBaseUrl;
exports.setBaseUrl = setBaseUrl;

//----------------------------------------------------------------------------

// The browser's IANA time zone (e.g. "America/New_York"), sent on every request
// as the X-Timezone header so the server can localize without each call body
// carrying it. Returns "" when the host can't resolve one (server falls back to
// its default). Computed here in the bundle, not index.html, because the page's
// CSP (script-src 'self') blocks inline scripts.
function getTimezone() {
    try {
        return Intl.DateTimeFormat().resolvedOptions().timeZone || "";
    } catch (e) {
        return "";
    }
};
exports.getTimezone = getTimezone;

//----------------------------------------------------------------------------

// POST a JSON body and unwrap the {code, msg, data} envelope. The JWT rides on
// the Authorization header unless noToken is set (login / verify). onerror is
// called as (msg, code, data) on an envelope failure, or (reason) on transport.
exports.post = function (uri, data, onsuccess, onerror, noToken) {
    var xhr = new XMLHttpRequest();
    xhr.open("POST", apiUrl(uri), true);
    xhr.setRequestHeader("Content-Type", "application/json; charset=utf-8");

    var token = noToken ? "" : getToken();
    if (token) {
        xhr.setRequestHeader("Authorization", "Bearer " + token);
    }
    var tz = getTimezone();
    if (tz) {
        xhr.setRequestHeader("X-Timezone", tz);
    }

    xhr.onreadystatechange = function () {
        if (xhr.readyState !== 4) {
            return;
        }

        if (xhr.status !== 200) {
            if (xhr.status === 401 && unauthorized()) { return; }
            if (onerror instanceof Function) {
                onerror("HTTP " + xhr.status, xhr.status);
            }
            return;
        }

        //----------------------------------------------------

        var json = null;
        try {
            json = JSON.parse(xhr.responseText);
        } catch (err) {
            if (onerror instanceof Function) {
                onerror(err);
            }
            return;
        }

        if (json.code !== 0) {
            if (onerror instanceof Function) {
                onerror(json.msg, json.code, json.data);
            }
            return;
        }

        if (onsuccess instanceof Function) {
            onsuccess(json.data);
        }
    };
    xhr.onerror = function () {
        if (onerror instanceof Function) {
            onerror("network error");
        }
    };

    xhr.send(data ? JSON.stringify(data) : null);
};

//----------------------------------------------------------------------------

// GET a JSON resource and unwrap the {code, msg, data} envelope. The JWT rides
// on the Authorization header. onerror is called as (msg, code, data) on an
// envelope/HTTP failure, or (reason) on transport failure.
exports.get = function (uri, onsuccess, onerror) {
    var xhr = new XMLHttpRequest();
    xhr.open("GET", apiUrl(uri), true);

    var token = getToken();
    if (token) {
        xhr.setRequestHeader("Authorization", "Bearer " + token);
    }
    var tz = getTimezone();
    if (tz) {
        xhr.setRequestHeader("X-Timezone", tz);
    }

    xhr.onreadystatechange = function () {
        if (xhr.readyState !== 4) {
            return;
        }
        if (xhr.status !== 200) {
            if (xhr.status === 401 && unauthorized()) { return; }
            if (onerror instanceof Function) {
                onerror("HTTP " + xhr.status, xhr.status);
            }
            return;
        }

        var json = null;
        try {
            json = JSON.parse(xhr.responseText);
        } catch (err) {
            if (onerror instanceof Function) {
                onerror(err);
            }
            return;
        }

        if (json.code !== 0) {
            if (onerror instanceof Function) {
                onerror(json.msg, json.code, json.data);
            }
            return;
        }
        if (onsuccess instanceof Function) {
            onsuccess(json.data);
        }
    };
    xhr.onerror = function () {
        if (onerror instanceof Function) {
            onerror("network error");
        }
    };

    xhr.send(null);
};

//----------------------------------------------------------------------------

// POST a JSON body and read the Server-Sent Events response incrementally with
// fetch + ReadableStream (a single request, unlike EventSource which can't POST
// a body). The JWT rides on the Authorization header. Each event's "data:"
// payload is handed to onmessage, except the terminal "[DONE]" (fires
// oncomplete) and an "event: error" frame (fires onerror with the payload).
// Returns an AbortController so the caller can cancel the stream.
exports.stream = function (uri, data, onmessage, oncomplete, onerror) {
    var controller = new AbortController();

    // A FormData body (file upload) is sent as multipart/form-data; the browser
    // sets that Content-Type with its boundary, so we must NOT set it ourselves.
    // Any other body is JSON-encoded.
    var isForm = (typeof FormData !== "undefined") && (data instanceof FormData);

    var headers = {};
    if (!isForm) {
        headers["Content-Type"] = "application/json; charset=utf-8";
    }
    var token = getToken();
    if (token) {
        headers["Authorization"] = "Bearer " + token;
    }
    var tz = getTimezone();
    if (tz) {
        headers["X-Timezone"] = tz;
    }

    var finished = false;
    function done(ok, reason) {
        if (finished) {
            return;
        }
        finished = true;
        if (ok) {
            if (oncomplete instanceof Function) {
                oncomplete();
            }
        } else if (onerror instanceof Function) {
            onerror(reason);
        }
    };

    //----------------------------------------------------

    // Parse one event block (lines up to a blank line): collect its "data:"
    // lines, route the terminal / error frames, hand the rest to onmessage.
    function dispatch(block) {
        var lines = block.split("\n");
        var payload = "";
        var event = "";
        for (var i = 0; i < lines.length; i ++) {
            if (lines[i].indexOf("data:") === 0) {
                payload += (payload ? "\n" : "") + lines[i].slice(5).replace(/^ /, "");
            } else if (lines[i].indexOf("event:") === 0) {
                event = lines[i].slice(6).trim();
            }
        }

        if (!payload) {
            return;
        }
        if (event === "error") {
            done(false, payload);
        } else if (payload === "[DONE]") {
            done(true);
        } else if (onmessage instanceof Function) {
            onmessage(payload);
        }
    };

    //----------------------------------------------------

    fetch(apiUrl(uri), {
        method  : "POST",
        headers : headers,
        body    : isForm ? data : JSON.stringify(data),
        signal  : controller.signal
    }).then(function (response) {
        // A rejected token bounces to login (handled centrally); end the stream
        // quietly rather than surfacing it as a stream error.
        if (response.status === 401 && unauthorized()) { finished = true; return; }
        if (!response.ok || !response.body) {
            throw "HTTP " + response.status;
        }

        var reader  = response.body.getReader();
        var decoder = new TextDecoder();
        var buffer  = "";

        function read() {
            return reader.read().then(function (result) {
                if (result.value) {
                    buffer += decoder.decode(result.value, { stream: true });

                    var idx;
                    while (!finished && (idx = buffer.indexOf("\n\n")) >= 0) {
                        dispatch(buffer.slice(0, idx));
                        buffer = buffer.slice(idx + 2);
                    }
                }

                if (finished) {
                    return;
                }
                if (result.done) {
                    done(true); // stream ended without an explicit [DONE]
                    return;
                }
                return read();
            });
        };

        return read();
    }).catch(function (e) {
        if (e && e.name === "AbortError") {
            return;
        }
        done(false, e);
    });

    return controller;
};

//----------------------------------------------------------------------------


//----------------------------------------------------------------------------
// Tanka QR-code sign-in panel, shown in place of the login card when the user
// taps "Continue with Tanka". The browser runs Tanka's own WASM request-signer
// (loaded from <base>/tanka-signer.js + <base>/tanka-sign.wasm), signs the
// create-QR / poll calls, and POSTs the signed {headers, body} to our backend,
// which proxies them to Tanka server-side (the browser can't reach Tanka's
// gateway directly -- CORS). On a confirmed scan the backend returns our usual
// auth envelope ({access_token,...}); we hand it to app.completeLogin and the
// app boots into chat.
//
// This mirrors data-mind's htdoc/src/login.js Tanka flow, adapted to mirobody's
// token-based, in-card login (not a full-page overlay gate).
//----------------------------------------------------------------------------

const ui   = require("./ui");
const net  = require("./net");
const i18n = require("./i18n");
const app  = require("./app");

const config = require("./config");
const color  = config.color;

var t = i18n.t;

// Tanka web-client constants + endpoints. The signature is computed over these
// paths, so they must match what the backend forwards to Tanka (TANKA_QRCODE_*).
const APPID = "E5kta86VM6", VERSION = "3.64.1", AIVERSION = "7";
const CREATE_PATH = "/npc/v2/common/qrcode/login";
const POLL_PATH   = "/npc/v1/user/requestQrCodeLogin";
const POLL_MS = 2000;

// Load <base>/qrcode.min.js once (sets global `qrcode`).
var qrLibPromise = null;
function loadQrLib() {
    if (qrLibPromise) { return qrLibPromise; }
    qrLibPromise = new Promise(function (resolve, reject) {
        if (typeof window.qrcode === "function") { return resolve(); }
        var s = document.createElement("script");
        s.src = net.appBase() + "/qrcode.min.js";
        s.onload = resolve;
        s.onerror = reject;
        document.head.appendChild(s);
    });
    return qrLibPromise;
};

// Load Tanka's WASM signer once (native dynamic import, kept out of the webpack
// bundle so unauthenticated visitors don't pay for it until they pick Tanka).
var signerPromise = null;
function getSigner() {
    if (!signerPromise) {
        signerPromise = import(/* webpackIgnore: true */ net.appBase() + "/tanka-signer.js")
            .then(function (m) { return m.initSigner(net.appBase() + "/tanka-sign.wasm"); });
    }
    return signerPromise;
};

//----------------------------------------------------------------------------

// Build the QR panel. `onBack` is invoked (after tearing down timers) when the
// user taps the back link, so the caller can re-render the login card.
function buildTankaPanel(onBack) {
    var TZ = net.getTimezone() || "Asia/Shanghai";
    var signer = null, deviceId = null, pollTimer = null, expireTimer = null, qrCodeData = null;

    var phone = config.isMobile();
    var card = ui.dom("section", {
        width         : "100%",
        maxWidth      : phone ? "100%" : "400px",
        margin        : phone ? "0 auto" : "auto",
        padding       : phone ? "24px 20px" : "32px 24px",
        display       : "flex",
        flexDirection : "column",
        alignItems    : "center",
        gap           : "16px",
        boxSizing     : "border-box",
        textAlign     : "center"
    });

    var title = ui.setText(ui.dom("h1", {
        fontFamily : "inherit",
        fontWeight : "600",
        fontSize   : "1.375rem",
        color      : color.onSurface,
        margin     : "0"
    }), t("tankaScanTitle"));

    // The QR box: a white tile that holds the generated <img>; a "spent" overlay
    // button replaces it once the code expires (tap to refresh).
    var reloadBtn = ui.dom("button", {
        display      : "none",
        position     : "absolute",
        top          : "50%", left: "50%", transform: "translate(-50%, -50%)",
        padding      : "10px 16px",
        border       : "none",
        borderRadius : "10px",
        background   : color.primary,
        color        : color.onPrimary,
        font         : "inherit",
        fontSize     : "0.85rem",
        fontWeight   : "600",
        cursor       : "pointer"
    }, { type: "button" });
    ui.setText(reloadBtn, t("tankaReload"));

    // Spinner shown while the QR is being prepared (loading the signer/qr lib +
    // the create-QR round trip). mb-spin keyframes live in index.css -- the CSP
    // blocks injected <style>, so inline styles may only reference them.
    var spinner = ui.dom("div", {
        width          : "34px",
        height         : "34px",
        border         : "3px solid " + color.outlineVar,
        borderTopColor : color.primary,
        borderRadius   : "50%",
        animation      : "mb-spin 0.8s linear infinite"
    });

    var qrBox = ui.dom("div", {
        position     : "relative",
        width        : "232px",
        height       : "232px",
        background   : "#ffffff",
        borderRadius : "20px",
        display      : "flex",
        alignItems   : "center",
        justifyContent : "center",
        boxShadow    : "0 10px 30px rgba(47,94,120,.14)",
        overflow     : "hidden"
    });
    qrBox.appendChild(spinner);
    qrBox.appendChild(reloadBtn);

    function makeStep(text) {
        var row = ui.dom("div", { display: "flex", alignItems: "center", gap: "10px" });
        var label = ui.setText(ui.dom("span", {
            color: color.onSurfaceVar, fontSize: "0.875rem"
        }), text);
        row.appendChild(label);
        return row;
    };
    var steps = ui.dom("div", {
        display       : "flex",
        flexDirection : "column",
        gap           : "8px",
        alignItems    : "flex-start",
        width         : "100%",
        maxWidth      : "300px",
        margin        : "0 auto"
    });
    steps.appendChild(makeStep("1. " + t("tankaStep1")));
    steps.appendChild(makeStep("2. " + t("tankaStep2")));
    steps.appendChild(makeStep("3. " + t("tankaStep3")));

    var status = ui.dom("p", {
        color     : color.onSurfaceVar,
        fontSize  : "0.8rem",
        minHeight : "1.2em",
        margin    : "0"
    });

    var backBtn = ui.dom("button", {
        border     : "none",
        background : "transparent",
        color      : color.primary,
        font       : "inherit",
        fontSize   : "0.875rem",
        fontWeight : "500",
        cursor     : "pointer",
        padding    : "4px"
    }, { type: "button" });
    ui.setText(backBtn, t("back"));

    card.appendChild(title);
    card.appendChild(qrBox);
    card.appendChild(steps);
    card.appendChild(status);
    card.appendChild(backBtn);

    //----------------------------------------------------
    // Status + lifecycle

    function setStatus(key, kind) {
        ui.setText(status, t(key));
        status.style.color = kind === "err" ? color.error
                           : kind === "ok"  ? color.primary
                           : color.onSurfaceVar;
    };

    // Stop the timers; called when the panel is torn down (back, expiry, sign-in)
    // or detected detached, so a stray interval never keeps polling.
    function stopTimers() {
        if (pollTimer)   { clearInterval(pollTimer);  pollTimer = null; }
        if (expireTimer) { clearTimeout(expireTimer); expireTimer = null; }
    };

    function detached() { return !document.body.contains(qrBox); }

    function expire() {
        stopTimers();
        spinner.style.display = "none";
        reloadBtn.style.display = "block";
        // Dim the QR underneath so it reads as spent.
        var img = qrBox.querySelector("img");
        if (img) { img.style.opacity = "0.2"; }
        setStatus("tankaExpired", "err");
    };

    function renderQR(url) {
        // Drop any previous QR image + un-dim, and clear the spinner.
        var old = qrBox.querySelector("img");
        if (old) { old.remove(); }
        spinner.style.display = "none";
        reloadBtn.style.display = "none";
        if (url && typeof window.qrcode === "function") {
            var qr = window.qrcode(0, "M");
            qr.addData(url);
            qr.make();
            var holder = document.createElement("div");
            holder.innerHTML = qr.createImgTag(6, 0);   // trusted: our own QR markup
            var img = holder.firstChild;
            img.style.width = "200px";
            img.style.height = "200px";
            img.style.imageRendering = "pixelated";
            qrBox.insertBefore(img, qrBox.firstChild);
        } else {
            setStatus("tankaQrErr", "err");
        }
    };

    //----------------------------------------------------
    // Tanka flow

    function signedHeaders(path, bodyStr) {
        var ts = String(Date.now());
        var sig = JSON.parse(signer.get_http_header(
            "POST", path, bodyStr, "", ts, TZ, deviceId, "7", "none", "none", AIVERSION, false));
        return {
            "Appid": APPID, "Version": VERSION, "Aiversion": AIVERSION, "Timezone": TZ,
            "Deviceid": deviceId, "Devicetype": "7", "Userid": "none", "Orgid": "none",
            "Timestamp": ts, "Cookie": "deviceId=" + deviceId,
            "x-t-sign": sig["x-t-sign"], "x-t-nonce": sig["x-t-nonce"],
            "x-t-sign-ver": sig["x-t-sign-ver"], "x-t-return-sign-result": "true"
        };
    };

    // POST a signed envelope to our backend proxy. onData/onErr mirror net.post.
    function proxy(apiPath, tankaPath, bodyObj, onData, onErr) {
        var bodyStr = JSON.stringify(bodyObj);
        net.post(apiPath, { headers: signedHeaders(tankaPath, bodyStr), body: bodyStr },
                 onData, onErr, true /* noToken */);
    };

    function poll() {
        if (detached()) { stopTimers(); return; }
        proxy("/tanka/poll", POLL_PATH, { qrCodeData: qrCodeData },
            function (data) {
                if (data && data.access_token) {
                    stopTimers();
                    setStatus("tankaSignedIn", "ok");
                    app.completeLogin(data.access_token);
                } else if (data && data.status === "expired") {
                    expire();
                } else {
                    setStatus("tankaWaiting");
                }
            },
            function () {
                // A non-zero envelope code (e.g. email unresolved) or transport
                // error: stop and let the user refresh.
                expire();
            });
    };

    function start() {
        setStatus("tankaLoading");
        spinner.style.display = "block";
        reloadBtn.style.display = "none";
        Promise.all([loadQrLib(), getSigner()]).then(function (vals) {
            signer = vals[1];
            deviceId = (window.crypto && window.crypto.randomUUID)
                ? window.crypto.randomUUID() : String(Math.random()).slice(2);
            proxy("/tanka/qrcode", CREATE_PATH,
                { qrCodeType: 0, deviceId: deviceId, deviceType: 7, createCodeStatus: 0 },
                function (data) {
                    if (!data || !data.qrCodeData) { setStatus("tankaStartErr", "err"); return; }
                    if (detached()) { return; }
                    qrCodeData = data.qrCodeData;
                    renderQR(qrCodeData);
                    setStatus("tankaWaiting");
                    stopTimers();
                    pollTimer = setInterval(poll, POLL_MS);
                    var ttl = msUntil(data.expireTime);
                    expireTimer = setTimeout(expire, ttl);
                },
                function () { setStatus("tankaStartErr", "err"); });
        }).catch(function (e) {
            setStatus("tankaSignerErr", "err");
            if (window.console) { console.error(e); }
        });
    };

    // Tanka's expireTime may be epoch-ms, epoch-s, or a TTL in seconds; normalize
    // to a ms-from-now delay, clamped so we always re-arm sensibly (default 5 min).
    function msUntil(expireTime) {
        var n = Number(expireTime), now = Date.now();
        if (!n) { return 300000; }
        if (n > 1e12) { return Math.max(5000, n - now); }       // epoch ms
        if (n > 1e9)  { return Math.max(5000, n * 1000 - now); } // epoch s
        return Math.max(5000, n * 1000);                         // TTL seconds
    };

    backBtn.addEventListener("click", function () {
        stopTimers();
        if (onBack instanceof Function) { onBack(); }
    });
    reloadBtn.addEventListener("click", start);

    start();
    return card;
};

// Warm the qr lib + the WASM signer (a native dynamic import + the ~552KB
// /tanka-sign.wasm bridge fetch, which also primes the server-side cache) while
// the user is still on the login card, so tapping "Continue with Tanka" shows
// the QR almost immediately -- only the create-QR round trip remains. Idempotent
// (both loaders cache their promise); failures are swallowed (the panel retries
// on open and surfaces the error there).
function preload() {
    loadQrLib().catch(function () {});
    getSigner().catch(function () {});
};

//----------------------------------------------------------------------------

exports.buildTankaPanel = buildTankaPanel;
exports.preload         = preload;
exports.loadQrLib       = loadQrLib;

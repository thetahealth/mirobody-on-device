
//----------------------------------------------------------------------------
// Web Google sign-in via the Firebase JS SDK (loaded from the gstatic CDN on
// first use, so the bundle stays small and unauthenticated visitors don't pay
// for it). A popup yields a Firebase ID token, which POST /firebase/verify
// validates server-side (FirebaseTokenValidator) and exchanges for app tokens.
//
// The Firebase web config comes from GET /firebase/verify (loadGoogleConfig);
// the button is shown only once a usable config (apiKey + projectId) arrives,
// so a server without Firebase configured simply hides Google sign-in.
//----------------------------------------------------------------------------

const net = require("./net");
const app = require("./app");

var FIREBASE_VERSION = "10.12.0";
var firebaseConfig = null;   // {apiKey, projectId, messagingSenderId} when available

// The Google button is shown iff GET /firebase/verify returned code 0 (i.e. the
// server has Google sign-in configured), which net.get signals via onsuccess.
function googleEnabled() {
    return !!firebaseConfig;
};

// Fetch the web config once, lazily, the first time the sign-in panel renders
// (not at page load, and never for signed-in users). On success (code 0),
// re-render the login view to reveal the button.
var googleConfigRequested = false;
function loadGoogleConfig() {
    if (googleConfigRequested) { return; }
    googleConfigRequested = true;
    net.get(
        "/firebase/verify",
        function (data) {
            firebaseConfig = (data && typeof data === "object") ? data : {};
            if (!net.getToken()) { app.render(); }
        },
        function () { /* non-zero code / error: leave Google sign-in hidden */ }
    );
};

// Resolve once to {authMod, auth}; reused across clicks.
var firebasePromise = null;
function loadFirebase() {
    if (firebasePromise) { return firebasePromise; }
    if (!googleEnabled()) { return Promise.reject(new Error("Google sign-in not configured")); }
    // The server doesn't send authDomain; use the Firebase default
    // "<projectId>.firebaseapp.com", which hosts the /__/auth/handler.
    var cfg = {
        apiKey            : firebaseConfig.apiKey,
        authDomain        : firebaseConfig.authDomain || (firebaseConfig.projectId + ".firebaseapp.com"),
        projectId         : firebaseConfig.projectId,
        messagingSenderId : firebaseConfig.messagingSenderId
    };
    firebasePromise = Promise.all([
        import(/* webpackIgnore: true */ "https://www.gstatic.com/firebasejs/" + FIREBASE_VERSION + "/firebase-app.js"),
        import(/* webpackIgnore: true */ "https://www.gstatic.com/firebasejs/" + FIREBASE_VERSION + "/firebase-auth.js")
    ]).then(function (mods) {
        var app = mods[0].initializeApp(cfg);
        return { authMod: mods[1], auth: mods[1].getAuth(app) };
    });
    return firebasePromise;
};

// Open the Google popup and resolve with a Firebase ID token.
function signInWithGoogle() {
    return loadFirebase().then(function (fb) {
        var provider = new fb.authMod.GoogleAuthProvider();
        return fb.authMod.signInWithPopup(fb.auth, provider);
    }).then(function (cred) {
        return cred.user.getIdToken();
    });
};

//----------------------------------------------------------------------------
// "Sign in with Apple" (web), a hybrid of two flows chosen by the OS:
//
//   * On Apple platforms (iOS / iPadOS / macOS) we use Apple's own JS SDK, which
//     presents the system-integrated sign-in sheet. It yields an Apple ID token
//     validated by POST /apple/verify (AppleTokenValidator). It authenticates
//     against the Services ID (GET /apple/verify -> {clientId}) and needs that
//     Services ID's return URL (https://<origin>/) registered + the domain
//     verified in the Apple Developer console.
//
//   * Everywhere else (Windows / Android / Linux browsers) we broker through
//     Firebase Auth's "apple.com" provider, exactly like the X button -- the
//     popup runs on Firebase's handler (https://<authDomain>/__/auth/handler,
//     the return URL registered for the Services ID inside Firebase), and the
//     Firebase ID token it yields is validated by POST /firebase/verify.
//
// signInWithApple() resolves to { token, native }: native=true means an Apple ID
// token bound for /apple/verify, native=false a Firebase ID token bound for
// /firebase/verify. The login view posts to the matching endpoint.
//
// The native iOS/Android apps use their platform SDKs + /apple/verify directly
// and don't touch any of this.

var APPLE_SDK_URL =
    "https://appleid.cdn-apple.com/appleauth/static/jsapi/appleid/1/en_US/appleid.auth.js";
var appleConfig = null;   // {clientId} (the Services ID) once GET /apple/verify resolves

// True on Apple operating systems (in any browser): iPhone/iPad/iPod, iPadOS 13+
// (which reports as "Mac" but is touch-capable), and macOS. This is an OS check,
// not a Safari check -- the native Apple JS flow works in any browser on these
// systems, and the system sheet is the reason to prefer it there.
function isApplePlatform() {
    var ua   = navigator.userAgent || "";
    var plat = navigator.platform  || "";
    if (/iPad|iPhone|iPod/.test(ua)) { return true; }
    if (/Mac/.test(plat) && navigator.maxTouchPoints > 1) { return true; }  // iPadOS 13+
    return /Macintosh|Mac OS X/.test(ua);
};

// The Apple button is shown whenever sign-in is actually possible here: the
// Firebase fallback covers every platform, and on Apple platforms the native
// flow also works once its Services ID config (GET /apple/verify) has loaded.
function appleEnabled() {
    return !!firebaseConfig || (isApplePlatform() && !!appleConfig);
};

// Fetch the Apple web config once, lazily. Resolves to {clientId} (or null when
// the server has no Apple Services ID configured). On a successful load it
// re-renders the login view so the button can appear if Firebase is absent.
var appleConfigPromise = null;
function loadAppleConfig() {
    if (appleConfigPromise) { return appleConfigPromise; }
    appleConfigPromise = new Promise(function (resolve) {
        net.get(
            "/apple/verify",
            function (data) {
                appleConfig = (data && typeof data === "object") ? data : {};
                if (!net.getToken()) { app.render(); }
                resolve(appleConfig);
            },
            function () { appleConfig = null; resolve(null); }
        );
    });
    return appleConfigPromise;
};

// Load Apple's JS SDK once (it attaches window.AppleID). Its script origin is
// allowed by the CSP only when Apple sign-in is configured (see server.cpp).
var appleSdkPromise = null;
function loadAppleSdk() {
    if (appleSdkPromise) { return appleSdkPromise; }
    appleSdkPromise = new Promise(function (resolve, reject) {
        if (window.AppleID && window.AppleID.auth) { resolve(window.AppleID); return; }
        var s = document.createElement("script");
        s.src   = APPLE_SDK_URL;
        s.async = true;
        s.onload = function () {
            if (window.AppleID && window.AppleID.auth) { resolve(window.AppleID); }
            else { reject(new Error("Apple SDK failed to load")); }
        };
        s.onerror = function () { reject(new Error("Apple SDK failed to load")); };
        document.head.appendChild(s);
    });
    return appleSdkPromise;
};

// Native flow: open Apple's own popup and resolve with an Apple ID token. The
// redirectURI must be a return URL registered for the Services ID.
function appleSignInNative() {
    return loadAppleSdk().then(function (AppleID) {
        AppleID.auth.init({
            clientId    : appleConfig.clientId,
            scope       : "name email",
            redirectURI : window.location.origin + window.location.pathname,
            usePopup    : true
        });
        return AppleID.auth.signIn();
    }).then(function (res) {
        var idToken = res && res.authorization && res.authorization.id_token;
        if (!idToken) { throw new Error("No Apple id_token"); }
        return idToken;
    });
};

// Firebase fallback: open the apple.com Firebase popup and resolve with a
// Firebase ID token (the same shape signInWithGoogle returns). email + name
// scopes are requested because the backend keys accounts by email.
function appleSignInFirebase() {
    return loadFirebase().then(function (fb) {
        var provider = new fb.authMod.OAuthProvider("apple.com");
        provider.addScope("email");
        provider.addScope("name");
        return fb.authMod.signInWithPopup(fb.auth, provider);
    }).then(function (cred) {
        return cred.user.getIdToken();
    });
};

// Pick the flow by OS and resolve to { token, native }: a native Apple ID token
// (-> /apple/verify) on Apple platforms with a Services ID configured, else a
// Firebase ID token (-> /firebase/verify).
function signInWithApple() {
    if (isApplePlatform()) {
        return loadAppleConfig().then(function (cfg) {
            if (cfg && cfg.clientId) {
                return appleSignInNative().then(function (t) { return { token: t, native: true }; });
            }
            return appleSignInFirebase().then(function (t) { return { token: t, native: false }; });
        });
    }
    return appleSignInFirebase().then(function (t) { return { token: t, native: false }; });
};

//----------------------------------------------------------------------------
// WeChat web sign-in. GET /wechat/verify returns {appid} (code 0) when WeChat
// web sign-in is configured, so the button is shown only then. Clicking it sends
// the browser to WeChat to obtain an OAuth `code`: inside WeChat's in-app
// browser via oauth2/authorize (snsapi_userinfo, a seamless redirect like the
// Google button), elsewhere via qrconnect (a desktop QR scan, snsapi_login).
// WeChat redirects back here with ?code=...&state=...; consumeWeChatRedirect()
// exchanges that code via POST /wechat/verify {flow:"web"} for app tokens.

// The OAuth `state` is stashed here between the outbound redirect and the
// bounce-back, so the callback can reject a forged or stale redirect.
var WECHAT_STATE_KEY = "mirobody-wechat-state";
var wechatConfig = null;   // {appid} when available
var wechatError  = false;  // set when a callback exchange fails; read by the login view

// Enabled whenever the probe succeeded (GET /wechat/verify returned code 0),
// mirroring googleEnabled() -- the server only returns code 0 when web sign-in
// is configured, so a non-null config is the signal to show the button.
function wechatEnabled() {
    return !!wechatConfig;
};

// Fetch the WeChat web config once, lazily, the first time the sign-in panel
// renders. On success (code 0), re-render the login view to reveal the button.
var wechatConfigRequested = false;
function loadWeChatConfig() {
    if (wechatConfigRequested) { return; }
    wechatConfigRequested = true;
    net.get(
        "/wechat/verify",
        function (data) {
            wechatConfig = (data && typeof data === "object") ? data : {};
            if (!net.getToken()) { app.render(); }
        },
        function () { /* not configured / error: leave WeChat sign-in hidden */ }
    );
};

// True when the page is loaded inside WeChat's in-app browser.
function inWeChat() {
    return /micromessenger/i.test(navigator.userAgent || "");
};

// Where WeChat returns to: this page stripped of any query/hash, so the
// bounce-back lands on the login route carrying just ?code=...&state=...
function wechatRedirectUri() {
    return window.location.origin + window.location.pathname;
};

// Begin WeChat sign-in by navigating to WeChat's authorize endpoint. Uses the
// in-app OAuth flow when inside WeChat, else the desktop QR flow.
function startWeChatLogin() {
    if (!wechatEnabled()) { return; }
    // A random state, persisted so the callback can verify the redirect is ours.
    var nonce = Math.random().toString(36).slice(2) + Date.now().toString(36);
    try { sessionStorage.setItem(WECHAT_STATE_KEY, nonce); } catch (e) { /* private mode */ }

    var redirect = encodeURIComponent(wechatRedirectUri());
    var appid    = encodeURIComponent(wechatConfig.appid);
    var state    = encodeURIComponent(nonce);
    var url = inWeChat()
        ? ("https://open.weixin.qq.com/connect/oauth2/authorize?appid=" + appid +
           "&redirect_uri=" + redirect + "&response_type=code&scope=snsapi_userinfo" +
           "&state=" + state + "#wechat_redirect")
        : ("https://open.weixin.qq.com/connect/qrconnect?appid=" + appid +
           "&redirect_uri=" + redirect + "&response_type=code&scope=snsapi_login" +
           "&state=" + state + "#wechat_redirect");
    window.location.href = url;
};

// Read ?code=&state= from the current URL (an OAuth callback), or null when
// absent. Shared by the redirect-based providers (WeChat web, GitHub), which
// both bounce back to this page with the same query shape.
function oauthCallback() {
    var params = new URLSearchParams(window.location.search || "");
    var code  = params.get("code");
    var state = params.get("state");
    return (code && state) ? { code: code, state: state } : null;
}

// Strip ?code=&state= from the address bar so a refresh can't replay a spent
// code. Called only by the provider that owns the matched state, so a callback
// meant for the other provider is left intact for its consumer.
function stripOAuthQuery() {
    if (window.history && window.history.replaceState) {
        window.history.replaceState(null, "", window.location.origin + window.location.pathname);
    }
};

// If this page load is a WeChat OAuth callback (?code=...&state=...) that WE
// issued, exchange the code for app tokens via POST /wechat/verify {flow:"web"}.
// The state must match the nonce we stashed before redirecting; a non-matching
// state (GitHub's callback, or a forgery) is left untouched for another consumer.
// On a match, strips the query so a reload can't replay the spent code, then on
// success sets the token and re-renders (-> chat); on failure flags wechatError
// and re-renders (-> login shows the error). Returns true when WE owned the
// callback.
function consumeWeChatRedirect() {
    var cb = oauthCallback();
    if (!cb) { return false; }

    var expected = "";
    try {
        expected = sessionStorage.getItem(WECHAT_STATE_KEY) || "";
    } catch (e) { /* private mode */ }

    // A state we didn't issue: not our redirect -- leave it for GitHub's consumer.
    if (!expected || expected !== cb.state) { return false; }
    try { sessionStorage.removeItem(WECHAT_STATE_KEY); } catch (e) { /* private mode */ }

    // Clear ?code=&state= from the address bar so a refresh can't re-run this.
    stripOAuthQuery();

    net.post(
        "/wechat/verify",
        { code: cb.code, flow: "web" },
        function (data) { // onsuccess
            if (data && data.access_token) {
                app.completeLogin(data.access_token);
            } else {
                wechatError = true;
                app.render();
            }
        },
        function () { // onerror
            wechatError = true;
            app.render();
        },
        true // noToken
    );
    return true;
};

// Read and clear the "last WeChat sign-in failed" flag (the login view shows a
// message when set).
function takeWeChatError() {
    var e = wechatError;
    wechatError = false;
    return e;
};

//----------------------------------------------------------------------------
// GitHub OAuth sign-in (web). GET /github/verify returns {clientId} (code 0)
// when GitHub sign-in is configured, so the button is shown only then. Clicking
// it redirects the browser to GitHub's authorize page; GitHub bounces back here
// with ?code=...&state=..., and consumeGitHubRedirect() exchanges that code via
// POST /github/verify for app tokens. Like WeChat web this is a full-page
// redirect (no popup/iframe, and the code exchange happens server-side), so it
// needs no extra CSP origins.

var GITHUB_STATE_KEY = "mirobody-github-state";
var githubConfig = null;   // {clientId} when available
var githubError  = false;  // set when a callback exchange fails; read by the login view

// Enabled whenever the probe succeeded (GET /github/verify returned code 0),
// mirroring wechatEnabled().
function githubEnabled() {
    return !!githubConfig;
};

// Fetch the GitHub web config once, lazily, the first time the sign-in panel
// renders. On success (code 0), re-render the login view to reveal the button.
var githubConfigRequested = false;
function loadGitHubConfig() {
    if (githubConfigRequested) { return; }
    githubConfigRequested = true;
    net.get(
        "/github/verify",
        function (data) {
            githubConfig = (data && typeof data === "object") ? data : {};
            if (!net.getToken()) { app.render(); }
        },
        function () { /* not configured / error: leave GitHub sign-in hidden */ }
    );
};

// Where GitHub returns to: this page stripped of any query/hash, so the
// bounce-back lands on the login route carrying just ?code=...&state=...
function githubRedirectUri() {
    return window.location.origin + window.location.pathname;
};

// Begin GitHub sign-in by navigating to GitHub's authorize endpoint. The
// read:user + user:email scope lets the server read the account's primary
// verified email via /user/emails during the code exchange.
function startGitHubLogin() {
    if (!githubEnabled()) { return; }
    // A random state, persisted so the callback can verify the redirect is ours.
    var nonce = Math.random().toString(36).slice(2) + Date.now().toString(36);
    try { sessionStorage.setItem(GITHUB_STATE_KEY, nonce); } catch (e) { /* private mode */ }

    var clientId = encodeURIComponent(githubConfig.clientId);
    var redirect = encodeURIComponent(githubRedirectUri());
    var state    = encodeURIComponent(nonce);
    window.location.href =
        "https://github.com/login/oauth/authorize?client_id=" + clientId +
        "&redirect_uri=" + redirect + "&scope=read:user%20user:email&state=" + state;
};

// If this page load is a GitHub OAuth callback (?code=...&state=...) that WE
// issued, exchange the code for app tokens via POST /github/verify. The state
// must match the nonce we stashed before redirecting; a non-matching state
// (WeChat's callback, or a forgery) is left untouched for the other consumer.
// On a match, strips the query so a reload can't replay the spent code. Returns
// true when WE owned the callback.
function consumeGitHubRedirect() {
    var cb = oauthCallback();
    if (!cb) { return false; }

    var expected = "";
    try {
        expected = sessionStorage.getItem(GITHUB_STATE_KEY) || "";
    } catch (e) { /* private mode */ }

    if (!expected || expected !== cb.state) { return false; }
    try { sessionStorage.removeItem(GITHUB_STATE_KEY); } catch (e) { /* private mode */ }

    stripOAuthQuery();

    net.post(
        "/github/verify",
        { code: cb.code },
        function (data) { // onsuccess
            if (data && data.access_token) {
                app.completeLogin(data.access_token);
            } else {
                githubError = true;
                app.render();
            }
        },
        function () { // onerror
            githubError = true;
            app.render();
        },
        true // noToken
    );
    return true;
};

// Read and clear the "last GitHub sign-in failed" flag (the login view shows a
// message when set).
function takeGitHubError() {
    var e = githubError;
    githubError = false;
    return e;
};

//----------------------------------------------------------------------------
// X (formerly Twitter) sign-in (web). Brokered through Firebase Auth's built-in
// "twitter.com" provider, exactly like the Google button above -- it reuses the
// same Firebase web config (GET /firebase/verify) and the same loadFirebase()
// helper, so there is no dedicated /x/verify route: the Firebase ID token from
// the popup is validated by the backend's FirebaseTokenValidator at
// /firebase/verify regardless of which provider produced it.
//
// X is therefore offered whenever Firebase is configured (same signal as the
// Google button). Enabling the "Twitter" provider in the Firebase console
// (Authentication -> Sign-in method) -- with "Request email address from users"
// turned on for the X app -- is what makes the popup actually succeed; the web
// client can't probe that, so a misconfigured provider surfaces as a failed
// sign-in rather than a hidden button.

// Shown whenever the Firebase web config is present (loaded by loadGoogleConfig,
// which the login view already calls), mirroring googleEnabled().
function xEnabled() {
    return !!firebaseConfig;
};

// Open the X popup via Firebase's twitter.com OAuthProvider and resolve with a
// Firebase ID token (the same shape signInWithGoogle returns).
function signInWithX() {
    return loadFirebase().then(function (fb) {
        var provider = new fb.authMod.OAuthProvider("twitter.com");
        return fb.authMod.signInWithPopup(fb.auth, provider);
    }).then(function (cred) {
        return cred.user.getIdToken();
    });
};

//----------------------------------------------------------------------------
// Tanka QR-code sign-in (web). GET /tanka/verify returns code 0 when Tanka login
// is enabled on the server, so the button is shown only then. Unlike the other
// providers there's no client-side SDK or redirect: clicking the button opens an
// in-card QR panel (see tanka.js) that talks to /tanka/qrcode + /tanka/poll.

var tankaConfig = null;   // {} (truthy) when enabled

function tankaEnabled() {
    return !!tankaConfig;
};

// Fetch the enablement flag once, lazily, the first time the sign-in panel
// renders. On success (code 0), re-render the login view to reveal the button.
var tankaConfigRequested = false;
function loadTankaConfig() {
    if (tankaConfigRequested) { return; }
    tankaConfigRequested = true;
    net.get(
        "/tanka/verify",
        function (data) {
            tankaConfig = (data && typeof data === "object") ? data : {};
            if (!net.getToken()) { app.render(); }
        },
        function () { /* not configured / error: leave Tanka sign-in hidden */ }
    );
};

//----------------------------------------------------------------------------

exports.googleEnabled    = googleEnabled;
exports.loadGoogleConfig = loadGoogleConfig;
exports.signInWithGoogle = signInWithGoogle;

exports.appleEnabled    = appleEnabled;
exports.loadAppleConfig = loadAppleConfig;
exports.signInWithApple = signInWithApple;

exports.wechatEnabled        = wechatEnabled;
exports.loadWeChatConfig     = loadWeChatConfig;
exports.startWeChatLogin     = startWeChatLogin;
exports.consumeWeChatRedirect = consumeWeChatRedirect;
exports.takeWeChatError      = takeWeChatError;

exports.githubEnabled         = githubEnabled;
exports.loadGitHubConfig      = loadGitHubConfig;
exports.startGitHubLogin      = startGitHubLogin;
exports.consumeGitHubRedirect = consumeGitHubRedirect;
exports.takeGitHubError       = takeGitHubError;

exports.xEnabled     = xEnabled;
exports.signInWithX  = signInWithX;

exports.tankaEnabled    = tankaEnabled;
exports.loadTankaConfig = loadTankaConfig;

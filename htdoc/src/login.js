
//----------------------------------------------------------------------------
// The sign-in view, styled to the Theta Health design: a centered serif brand
// block, the configured social providers, then email + one-time-code (a plain
// code field with an inline "Send code" and a navy "Sign in").
//----------------------------------------------------------------------------

const ui   = require("./ui");
const net  = require("./net");
const i18n = require("./i18n");
const app  = require("./app");

const config = require("./config");
const color  = config.color;
const serifFamily = config.serifFamily;
const state  = config.state;
const isMobile = config.isMobile;

const widgets = require("./widgets");
const field   = widgets.field;
const button  = widgets.button;
const brandMark   = widgets.brandMark;
const setDisabled = widgets.setDisabled;

const icons = require("./icons");
const GOOGLE_SVG = icons.GOOGLE_SVG;
const APPLE_SVG  = icons.APPLE_SVG;
const WECHAT_SVG = icons.WECHAT_SVG;
const GITHUB_SVG = icons.GITHUB_SVG;
const X_SVG      = icons.X_SVG;
const TANKA_SVG  = icons.TANKA_SVG;

const auth = require("./auth");
const tanka = require("./tanka");

var t = i18n.t;

// Height-aware layout tiers, so the login card fits a laptop screen without
// scrolling. Tier 1 (compactFit): providers render as a compact icon row instead
// of a labeled stack -- also forced when the screen is narrow (isMobile). Tier 2
// (shortFit): the icon row alone wasn't enough, so shrink the brand block (logo,
// title, margins) and tighten the card spacing too. Neither case can be known
// until the card is laid out, so overflow is measured after render and latched
// here; a window resize clears both so a taller window can restore the full
// layout.
var compactFit   = false;
var shortFit     = false;
var resizeHooked = false;

// The address the last verification code was sent to (lowercased); the code
// field stays locked until it matches the address in the email field.
// Module-level so mid-login re-renders (the compact reflow, a language switch)
// don't re-lock a field the user can already fill.
var codeSentTo = "";

// Draw the APK download QR into `box` once the QR lib has loaded. Mirrors the
// QR rendering in tanka.js (our own trusted markup).
function renderApkQr(box, url) {
    if (typeof window.qrcode !== "function") { return; }
    var qr = window.qrcode(0, "M");
    qr.addData(url);
    qr.make();
    var holder = document.createElement("div");
    holder.innerHTML = qr.createImgTag(4, 0);   // trusted: our own QR markup
    var img = holder.firstChild;
    img.style.width = "100%";
    img.style.height = "100%";
    img.style.imageRendering = "pixelated";
    box.appendChild(img);
};

// Small dialog with the APK download QR, opened from the one-line link on the
// login card -- kept out of the card itself so the QR's height never pushes the
// login page past a laptop viewport. Same backdrop/card pattern as modals.js;
// dismisses on the Close button or a backdrop click.
function showApkQrModal(url) {
    var backdrop = ui.dom("div", {
        position: "fixed", inset: "0", background: "rgba(0, 0, 0, 0.4)",
        display: "flex", alignItems: "center", justifyContent: "center",
        padding: "16px", zIndex: "1200"
    });
    var card = ui.dom("div", {
        background: color.background, borderRadius: "14px", padding: "20px 22px",
        boxShadow: "0 8px 32px rgba(0, 0, 0, 0.25)",
        display: "flex", flexDirection: "column", alignItems: "center", gap: "14px"
    });
    card.appendChild(ui.setText(ui.dom("div", {
        fontSize: "0.9rem", color: color.onSurface, textAlign: "center"
    }), t("downloadAndroidQr")));
    var box = ui.dom("div", {
        width: "168px", height: "168px", padding: "8px", boxSizing: "border-box",
        background: "#fff", borderRadius: "8px"
    });
    card.appendChild(box);
    tanka.loadQrLib().then(function () { renderApkQr(box, url); });

    function dismiss() {
        if (backdrop.parentNode) { backdrop.parentNode.removeChild(backdrop); }
    };
    var closeBtn = button(t("close"), true);
    closeBtn.addEventListener("click", dismiss);
    card.appendChild(closeBtn);

    backdrop.addEventListener("click", function (evt) {
        if (evt.target === backdrop) { dismiss(); }
    });
    backdrop.appendChild(card);
    document.body.appendChild(backdrop);
};

//----------------------------------------------------------------------------

function buildLogin() {
    app.slots.topCenter = null;
    // Settings (language / font / appearance / backend) stay reachable before
    // login through the top-left hamburger: signed out, the nav drawer narrows to just
    // that group (see history.js). This screen adds no bar actions of its own.

    // Probe which social providers are configured (only the sign-in panel needs
    // it); each loader re-renders the view when its config arrives.
    auth.loadGoogleConfig();
    auth.loadAppleConfig();
    auth.loadWeChatConfig();
    auth.loadGitHubConfig();
    auth.loadTankaConfig();

    // Phone: a full-width, top-aligned column. Wide screens: a narrow, vertically
    // centered card.
    var phone = isMobile();
    // `compact` drives only the provider layout (icon row vs labeled stack); the
    // card sizing stays purely width-based on `phone`. `shortMode` is the tier-2
    // height fallback (desktop only -- the phone layout scrolls naturally).
    var compact = phone || compactFit;
    var shortMode = !phone && shortFit;
    var card = ui.dom("section", {
        width         : "100%",
        maxWidth      : phone ? "100%" : "400px",
        margin        : phone ? "0 auto" : "auto",
        padding       : phone ? "16px 20px 40px" : (shortMode ? "8px 24px 16px" : "8px 24px 40px"),
        display       : "flex",
        flexDirection : "column",
        gap           : shortMode ? "10px" : "14px",
        boxSizing     : "border-box"
    });

    // -- centered brand block: logo + serif title + subtitle ----------------
    // In shortMode everything steps down one size so the whole card can fit a
    // short laptop viewport: smaller logo, smaller title, tighter margins.
    var brandBlock = ui.dom("div", {
        display: "flex", flexDirection: "column", alignItems: "center",
        textAlign: "center", gap: "0",
        margin: shortMode ? "4px 0 14px" : "12px 0 28px"
    });
    brandBlock.appendChild(brandMark(shortMode ? "44px" : "60px",
        { marginBottom: shortMode ? "10px" : "16px" }));
    brandBlock.appendChild(ui.setText(ui.dom("h1", {
        fontFamily: serifFamily, fontWeight: "600",
        // large display title, like the design; one step smaller in shortMode
        fontSize: shortMode ? "clamp(2rem, 7vw, 2.5rem)" : "clamp(2.75rem, 10vw, 3.75rem)",
        lineHeight: "1.04", letterSpacing: "-0.5px",
        color: color.wordmark, margin: "0"           // brand-title ink (white in dark)
    }), "Mirobody"));
    brandBlock.appendChild(ui.setText(ui.dom("p", {
        color: color.onSurfaceVar, fontSize: "1.05rem", lineHeight: "1.4",
        margin: shortMode ? "8px 0 0" : "14px 0 0"
    }), t("loginContinue")));
    card.appendChild(brandBlock);

    // Shared status line (every flow writes here).
    var status = ui.dom("p", {
        color: color.onSurfaceVar, fontSize: "0.8rem",
        minHeight: "1.2em", margin: "2px 0 0", textAlign: "center"
    });

    //----------------------------------------------------
    // Social providers. On phone they'd stack into a tall column of "Sign in
    // with ..." rows, so there they collapse to a compact row of icon-only
    // buttons (label kept as title/aria-label); wide screens keep the labeled
    // full-width buttons. `iconColor` tints a single-color mark (the multi-color
    // Google "G" carries its own fills and ignores it).

    // Holds the provider buttons: a compact icon row, or a stacked labeled column.
    // Appended to the card later (before the divider) only if any provider exists.
    var socialRow = ui.dom("div", compact
        ? { display: "flex", flexWrap: "wrap", justifyContent: "center", gap: "12px" }
        : { display: "flex", flexDirection: "column", gap: "14px" });

    function oauthButton(iconSvg, label, iconColor) {
        var btn = ui.dom("button", compact ? {
            width: "52px", height: "52px", flex: "0 0 auto", display: "flex",
            alignItems: "center", justifyContent: "center",
            border: "1px solid " + color.outlineVar, borderRadius: "12px",
            background: color.surfaceLow, color: color.onSurface, cursor: "pointer"
        } : {
            width: "100%", height: "52px", display: "flex",
            alignItems: "center", justifyContent: "center", gap: "10px",
            border: "1px solid " + color.outlineVar, borderRadius: "12px",
            background: color.surfaceLow, color: color.onSurface,
            font: "inherit", fontSize: "0.9rem", fontWeight: "500", cursor: "pointer"
        }, { type: "button", title: label, "aria-label": label });
        var icon = ui.dom("span", { display: "flex", color: iconColor || "inherit" });
        ui.setHTML(icon, iconSvg);
        btn.appendChild(icon);
        if (!compact) { btn.appendChild(ui.setText(ui.dom("span", null), label)); }
        return btn;
    };

    if (auth.googleEnabled()) {
        var googleBtn = oauthButton(GOOGLE_SVG, t("continueWithGoogle"));
        googleBtn.addEventListener("click", function () {
            googleBtn.disabled = true;
            ui.setText(status, "");
            auth.signInWithGoogle().then(function (idToken) {
                net.post("/firebase/verify", { token: idToken },
                    function (data) {
                        if (data && data.access_token) { app.completeLogin(data.access_token); }
                        else { ui.setText(status, t("googleFailed")); googleBtn.disabled = false; }
                    },
                    function (msg) {
                        ui.setText(status, ui.isString(msg) ? msg : t("googleFailed"));
                        googleBtn.disabled = false;
                    },
                    true);
            }).catch(function () {
                ui.setText(status, t("googleFailed"));
                googleBtn.disabled = false;
            });
        });
        socialRow.appendChild(googleBtn);
    }

    if (auth.appleEnabled()) {
        // signInWithApple picks the flow by OS: Apple's own popup (native Apple ID
        // token -> /apple/verify) on Apple platforms, else a Firebase popup
        // (apple.com -> /firebase/verify). The result says which.
        var appleBtn = oauthButton(APPLE_SVG, t("continueWithApple"), color.onSurface);
        appleBtn.addEventListener("click", function () {
            appleBtn.disabled = true;
            ui.setText(status, "");
            auth.signInWithApple().then(function (r) {
                var url  = r.native ? "/apple/verify" : "/firebase/verify";
                var body = r.native ? { id_token: r.token } : { token: r.token };
                net.post(url, body,
                    function (data) {
                        if (data && data.access_token) { app.completeLogin(data.access_token); }
                        else { ui.setText(status, t("appleFailed")); appleBtn.disabled = false; }
                    },
                    function (msg) {
                        ui.setText(status, ui.isString(msg) ? msg : t("appleFailed"));
                        appleBtn.disabled = false;
                    },
                    true);
            }).catch(function () {
                ui.setText(status, t("appleFailed"));
                appleBtn.disabled = false;
            });
        });
        socialRow.appendChild(appleBtn);
    }

    if (auth.wechatEnabled()) {
        // Clicking redirects to WeChat (QR on desktop, in-app OAuth inside WeChat);
        // auth.js handles the bounce-back.
        var wechatBtn = oauthButton(WECHAT_SVG, t("continueWithWeChat"), "#07C160");
        wechatBtn.addEventListener("click", function () {
            wechatBtn.disabled = true;
            ui.setText(status, "");
            auth.startWeChatLogin();
        });
        socialRow.appendChild(wechatBtn);
    }

    if (auth.githubEnabled()) {
        // Clicking redirects to GitHub's authorize page; auth.js handles the
        // bounce-back exchange.
        var githubBtn = oauthButton(GITHUB_SVG, t("continueWithGitHub"), color.onSurface);
        githubBtn.addEventListener("click", function () {
            githubBtn.disabled = true;
            ui.setText(status, "");
            auth.startGitHubLogin();
        });
        socialRow.appendChild(githubBtn);
    }

    if (auth.xEnabled()) {
        // Like Google this opens a Firebase popup; the Firebase ID token is
        // verified by POST /firebase/verify (any provider accepted).
        var xBtn = oauthButton(X_SVG, t("continueWithX"), color.onSurface);
        xBtn.addEventListener("click", function () {
            xBtn.disabled = true;
            ui.setText(status, "");
            auth.signInWithX().then(function (idToken) {
                net.post("/firebase/verify", { token: idToken },
                    function (data) {
                        if (data && data.access_token) { app.completeLogin(data.access_token); }
                        else { ui.setText(status, t("xFailed")); xBtn.disabled = false; }
                    },
                    function (msg) {
                        ui.setText(status, ui.isString(msg) ? msg : t("xFailed"));
                        xBtn.disabled = false;
                    },
                    true);
            }).catch(function () {
                ui.setText(status, t("xFailed"));
                xBtn.disabled = false;
            });
        });
        socialRow.appendChild(xBtn);
    }

    if (auth.tankaEnabled()) {
        // Tanka is a QR-scan flow: clicking swaps the whole card for an in-card QR
        // panel (tanka.js), which polls the backend and calls app.completeLogin on
        // a confirmed scan. Back re-renders login.
        var tankaBtn = oauthButton(TANKA_SVG, t("continueWithTanka"));
        tankaBtn.addEventListener("click", function () {
            card.replaceWith(tanka.buildTankaPanel(function () { app.render(); }));
        });
        socialRow.appendChild(tankaBtn);
        // Warm the signer + qr lib now so the QR appears almost instantly.
        tanka.preload();
    }

    // Mount the provider row (above the divider/email form) only when populated.
    if (socialRow.childNodes.length) { card.appendChild(socialRow); }

    // "OR" divider between the social block and the email form (only when at least
    // one provider is shown).
    if (auth.googleEnabled() || auth.appleEnabled() || auth.wechatEnabled() ||
        auth.githubEnabled() || auth.xEnabled() || auth.tankaEnabled()) {
        var hairline = function () {
            return ui.dom("div", { flex: "1 1 0", height: "1px", background: color.outlineVar });
        };
        var divider = ui.dom("div", {
            display: "flex", alignItems: "center", gap: "12px", margin: "4px 0"
        });
        divider.appendChild(hairline());
        divider.appendChild(ui.setText(ui.dom("span", {
            fontSize: "0.75rem", color: color.onSurfaceVar, textTransform: "uppercase",
            letterSpacing: "0.6px"
        }), t("authOr")));
        divider.appendChild(hairline());
        card.appendChild(divider);
    }

    //----------------------------------------------------
    // Email + one-time code.

    function emailValid(v) {
        // Full shape check -- at least *@*.*: an '@' with non-empty, space-free
        // parts on both sides, and the domain must contain a dot. Stricter than
        // the server's lenient normalize_email (which would accept single-label
        // domains like "user289@demo"), so typos die here instead of costing a
        // sent code.
        return /^[^\s@]+@[^\s@]+\.[^\s@]+$/.test((v || "").trim());
    };

    var emailInput = field({ type: "email", placeholder: t("emailPlaceholder"), autocomplete: "email" });
    emailInput.style.background = color.surfaceLow;
    // Survive a re-render (e.g. the resize-driven compact reflow) by mirroring the
    // field into state and seeding it back on rebuild.
    emailInput.value = state.email || "";
    emailInput.addEventListener("input", function () { state.email = emailInput.value.trim(); });

    var codeInput = field({
        type: "text", inputmode: "numeric", maxlength: "6",
        placeholder: t("verificationCode"), autocomplete: "one-time-code"
    });
    codeInput.style.background = color.surfaceLow;
    codeInput.style.flex = "1 1 0";
    codeInput.style.minWidth = "0";

    var sendBtn   = button(t("sendCode"), false);   // outlined / secondary
    var signInBtn = button(t("loginTitle"), true);  // navy / primary
    ui.setStyle(sendBtn, { flex: "0 0 auto", whiteSpace: "nowrap" });
    ui.setStyle(signInBtn, { width: "100%", height: "52px", marginTop: "2px" });

    var codeRow = ui.dom("div", { display: "flex", gap: "10px", alignItems: "stretch" });
    codeRow.appendChild(codeInput);
    codeRow.appendChild(sendBtn);

    // Resend cooldown: after a code is sent, disable Send code and tick down a
    // countdown before allowing another send.
    var sending = false;
    var verifying = false;
    var COOLDOWN_SECONDS = 60;
    var cooldownTimer = null;
    function startCooldown(seconds) {
        var remaining = seconds;
        setDisabled(sendBtn, true);
        ui.setText(sendBtn, t("resendIn", remaining));
        if (cooldownTimer) { clearInterval(cooldownTimer); }
        cooldownTimer = setInterval(function () {
            if (!document.body.contains(sendBtn)) {   // a re-render replaced it
                clearInterval(cooldownTimer); cooldownTimer = null; return;
            }
            remaining -= 1;
            if (remaining <= 0) {
                clearInterval(cooldownTimer); cooldownTimer = null;
                ui.setText(sendBtn, t("resendCode"));
                refreshEmailGate();
            } else {
                ui.setText(sendBtn, t("resendIn", remaining));
            }
        }, 1000);
    };

    // Staged flow, each step unlocking the next: 1) a valid-looking address
    // unlocks "Send code"; 2) a successful send unlocks the code field (editing
    // the address re-locks it until a code is sent to the new one); 3) all six
    // digits unlock Sign in (six digits also auto-submit). Send code has two
    // further states that own the button themselves -- a request in flight, and
    // the resend cooldown.
    function codeSent() {
        return !!codeSentTo && codeSentTo === emailInput.value.trim().toLowerCase();
    };
    function refreshEmailGate() {
        var ok = emailValid(emailInput.value);
        setDisabled(codeInput, !ok || !codeSent());
        refreshSignInBtn();
        if (sending || cooldownTimer) { return; }
        setDisabled(sendBtn, !ok);
    };
    // Sign in needs the whole staircase: a valid address, a code sent to it, and
    // all six digits.
    function refreshSignInBtn() {
        setDisabled(signInBtn, !emailValid(emailInput.value) || !codeSent()
            || codeInput.value.length !== 6);
    };
    emailInput.addEventListener("input", refreshEmailGate);
    refreshEmailGate();

    sendBtn.addEventListener("click", function () {
        var email = emailInput.value.trim();
        if (!emailValid(email)) { ui.setText(status, t("emailRequired")); return; }

        sending = true;
        setDisabled(sendBtn, true);
        ui.setText(status, t("sendingCode"));

        net.post("/email/login", { email: email },
            function () {
                sending = false;
                state.email = email;
                codeSentTo = email.toLowerCase();
                ui.setText(status, t("codeSentTo", email));
                startCooldown(COOLDOWN_SECONDS);
                refreshEmailGate();   // unlock the code field before focusing it
                codeInput.focus();
            },
            function (msg) {
                sending = false;
                ui.setText(status, ui.isString(msg) ? msg : t("sendFailed"));
                refreshEmailGate();
            },
            true);
    });

    function verify() {
        var code = codeInput.value.trim();
        if (!code) { ui.setText(status, t("enterCode")); return; }
        if (verifying) { return; }

        verifying = true;
        ui.setText(status, t("verifying"));
        net.post("/email/verify", { email: state.email || emailInput.value.trim(), code: code },
            function (data) {
                if (!data || !data.access_token) {
                    verifying = false;
                    ui.setText(status, t("badResponse"));
                    return;
                }
                app.completeLogin(data.access_token);
            },
            function (msg) {
                verifying = false;
                ui.setText(status, ui.isString(msg) ? msg : t("verifyFailed"));
                codeInput.style.borderColor = color.error;
            },
            true);
    };

    // Digits only; clear the error tint on edit; auto-submit once six are entered.
    codeInput.addEventListener("input", function () {
        codeInput.value = codeInput.value.replace(/\D/g, "").slice(0, 6);
        codeInput.style.borderColor = color.outlineVar;
        refreshSignInBtn();
        if (codeInput.value.length === 6) { verify(); }
    });
    // Enter in the email field sends the code; Enter in the code field verifies.
    emailInput.addEventListener("keydown", function (evt) {
        if (evt.key === "Enter") { evt.preventDefault(); if (!sendBtn.disabled) { sendBtn.click(); } }
    });
    codeInput.addEventListener("keydown", function (evt) {
        if (evt.key === "Enter") { evt.preventDefault(); if (!signInBtn.disabled) { verify(); } }
    });
    signInBtn.addEventListener("click", verify);

    card.appendChild(emailInput);
    card.appendChild(codeRow);
    card.appendChild(signInBtn);
    card.appendChild(status);

    // The Android APK — served from the doc root (htdoc/static/mirobody.apk,
    // copied to res/htdoc/ at build). Only the Android app exists, so: on Android,
    // a tap-to-download link (installs directly); on desktop, a one-line link
    // that pops the scan-to-download QR in a dialog (inline, the 128px QR pushed
    // the card past a laptop viewport's height); on other mobiles (iPhone etc.)
    // nothing -- an APK can't be installed there, and you can't scan your own
    // screen.
    var apkUrl = net.appBase() + "/mirobody.apk";
    if (config.isAndroid()) {
        var apkLink = ui.dom("a", {
            display: "block", textAlign: "center", marginTop: "20px",
            fontSize: "0.8rem", color: color.onSurfaceVar, textDecoration: "none"
        }, { href: apkUrl, download: "mirobody.apk" });
        ui.setText(apkLink, t("downloadAndroid"));
        card.appendChild(apkLink);
    } else if (!config.isMobile()) {
        var apkQrLink = ui.dom("button", {
            border: "none", background: "transparent", cursor: "pointer",
            display: "block", margin: (shortMode ? "12px" : "20px") + " auto 0",
            padding: "0", font: "inherit", fontSize: "0.8rem",
            color: color.onSurfaceVar, textDecoration: "underline"
        }, { type: "button" });
        ui.setText(apkQrLink, t("downloadAndroid"));
        apkQrLink.addEventListener("click", function () { showApkQrModal(apkUrl); });
        card.appendChild(apkQrLink);
        // Warm the QR lib now so the dialog draws instantly when opened.
        tanka.loadQrLib();
    }

    // Surface a failed WeChat / GitHub callback exchange (flagged by auth.js before
    // it re-rendered the login view).
    if (auth.takeWeChatError()) { ui.setText(status, t("wechatFailed")); }
    if (auth.takeGitHubError()) { ui.setText(status, t("githubFailed")); }

    // Height-aware fallback: once mounted, if the card overflows the viewport,
    // escalate one tier and re-render -- labeled stack -> compact icon row ->
    // shortMode (smaller brand block, tighter spacing). Each step only shrinks
    // and shortMode is the floor (past it the page just scrolls), so the
    // escalation converges without looping.
    if (!phone) {
        requestAnimationFrame(function () {
            if (!app.showingLogin()) { return; }   // view changed before the frame
            if (document.documentElement.scrollHeight <= window.innerHeight + 1) { return; }
            if (!compactFit)    { compactFit = true; app.render(); }
            else if (!shortFit) { shortFit = true;   app.render(); }
        });
    }

    // Re-evaluate on resize: drop both latches so a now-taller window restores
    // the full layout (the check above re-trips them tier by tier if it still
    // overflows). Debounced, and only while the login view is up.
    //
    // Width-only guard: on touch devices, focusing an input pops the soft keyboard,
    // which shrinks the viewport HEIGHT and fires `resize`. Re-rendering here would
    // clear #app and recreate the focused input -- blurring it and dismissing the
    // keyboard before the user can type. The compact-fit reflow only ever depends
    // on width (isMobile) or a genuinely taller window, so ignore resizes that
    // don't change the width; the keyboard never changes it.
    if (!resizeHooked) {
        resizeHooked = true;
        var pending = null;
        var lastWidth = window.innerWidth;
        window.addEventListener("resize", function () {
            if (!app.showingLogin()) { return; }   // not on the login view
            if (window.innerWidth === lastWidth) { return; }   // height-only (soft keyboard): ignore
            lastWidth = window.innerWidth;
            if (pending) { clearTimeout(pending); }
            pending = setTimeout(function () {
                pending = null;
                compactFit = false;
                shortFit   = false;
                app.render();
            }, 150);
        });
    }

    return card;
};

//----------------------------------------------------------------------------

exports.buildLogin = buildLogin;

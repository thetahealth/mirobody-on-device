
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

const icons = require("./icons");
const GOOGLE_SVG = icons.GOOGLE_SVG;
const APPLE_SVG  = icons.APPLE_SVG;
const WECHAT_SVG = icons.WECHAT_SVG;
const GITHUB_SVG = icons.GITHUB_SVG;
const X_SVG      = icons.X_SVG;
const TANKA_SVG  = icons.TANKA_SVG;

const auth = require("./auth");
const tanka = require("./tanka");
const buildSettingsMenu = require("./topbar").buildSettingsMenu;

const logoUrl = require("./assets/mirobody.svg");

var t = i18n.t;

// Providers render as a compact icon row (not a labeled stack) when the screen
// is narrow (isMobile) OR too short to fit the stack without scrolling. The
// height case can't be known until the card is laid out, so it's measured after
// render and latched here; a window resize clears it so a taller window can
// restore the labels.
var compactFit   = false;
var resizeHooked = false;

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

//----------------------------------------------------------------------------

function buildLogin() {
    app.slots.topCenter = null;
    // Settings (language / font / backend / about) stays available before login;
    // it hides Sign out while signed out (see buildSettingsMenu).
    app.slots.topRight = buildSettingsMenu();

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
    // card sizing stays purely width-based on `phone`.
    var compact = phone || compactFit;
    var card = ui.dom("section", {
        width         : "100%",
        maxWidth      : phone ? "100%" : "400px",
        margin        : phone ? "0 auto" : "auto",
        padding       : phone ? "16px 20px 40px" : "8px 24px 40px",
        display       : "flex",
        flexDirection : "column",
        gap           : "14px",
        boxSizing     : "border-box"
    });

    // -- centered brand block: logo + serif title + subtitle ----------------
    var brandBlock = ui.dom("div", {
        display: "flex", flexDirection: "column", alignItems: "center",
        textAlign: "center", gap: "0", margin: "12px 0 28px"
    });
    brandBlock.appendChild(ui.img(logoUrl, { width: "60px", height: "60px", marginBottom: "16px" }));
    brandBlock.appendChild(ui.setText(ui.dom("h1", {
        fontFamily: serifFamily, fontWeight: "600",
        fontSize: "clamp(2.75rem, 10vw, 3.75rem)",   // large display title, like the design
        lineHeight: "1.04", letterSpacing: "-0.5px",
        color: "#0f1115", margin: "0"                // near-pure black
    }), "Mirobody"));
    brandBlock.appendChild(ui.setText(ui.dom("p", {
        color: color.onSurfaceVar, fontSize: "1.05rem", lineHeight: "1.4",
        margin: "14px 0 0"
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
        var appleBtn = oauthButton(APPLE_SVG, t("continueWithApple"), "#000000");
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
        var githubBtn = oauthButton(GITHUB_SVG, t("continueWithGitHub"), "#000000");
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
        var xBtn = oauthButton(X_SVG, t("continueWithX"), "#000000");
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
        // Match the server's lenient rule (normalize_email): an '@' with non-empty,
        // space-free parts on both sides. No dot required, so single-label demo
        // domains like "user289@demo" are accepted.
        return /^[^\s@]+@[^\s@]+$/.test((v || "").trim());
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
        sendBtn.disabled = true;
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
                refreshSendBtn();
            } else {
                ui.setText(sendBtn, t("resendIn", remaining));
            }
        }, 1000);
    };

    // Send code is enabled only for a valid-looking email, except while a request
    // is in flight or the cooldown is running (those states own the button).
    function refreshSendBtn() {
        if (sending || cooldownTimer) { return; }
        sendBtn.disabled = !emailValid(emailInput.value);
    };
    emailInput.addEventListener("input", refreshSendBtn);
    refreshSendBtn();

    sendBtn.addEventListener("click", function () {
        var email = emailInput.value.trim();
        if (!emailValid(email)) { ui.setText(status, t("emailRequired")); return; }

        sending = true;
        sendBtn.disabled = true;
        ui.setText(status, t("sendingCode"));

        net.post("/email/login", { email: email },
            function () {
                sending = false;
                state.email = email;
                ui.setText(status, t("codeSentTo", email));
                startCooldown(COOLDOWN_SECONDS);
                codeInput.focus();
            },
            function (msg) {
                sending = false;
                ui.setText(status, ui.isString(msg) ? msg : t("sendFailed"));
                refreshSendBtn();
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
        if (codeInput.value.length === 6) { verify(); }
    });
    // Enter in the email field sends the code; Enter in the code field verifies.
    emailInput.addEventListener("keydown", function (evt) {
        if (evt.key === "Enter") { evt.preventDefault(); if (!sendBtn.disabled) { sendBtn.click(); } }
    });
    codeInput.addEventListener("keydown", function (evt) {
        if (evt.key === "Enter") { evt.preventDefault(); verify(); }
    });
    signInBtn.addEventListener("click", verify);

    card.appendChild(emailInput);
    card.appendChild(codeRow);
    card.appendChild(signInBtn);
    card.appendChild(status);

    // The Android APK — served from the doc root (htdoc/static/mirobody.apk,
    // copied to res/htdoc/ at build). On Android, a plain download link installs
    // it directly. On PC the APK can't be installed locally, so show a QR code
    // that points to the download URL for the user to scan with their phone.
    var apkUrl = net.appBase() + "/mirobody.apk";
    if (config.isAndroid()) {
        var apkLink = ui.dom("a", {
            display: "block", textAlign: "center", marginTop: "20px",
            fontSize: "0.8rem", color: color.onSurfaceVar, textDecoration: "none"
        }, { href: apkUrl, download: "mirobody.apk" });
        ui.setText(apkLink, t("downloadAndroid"));
        card.appendChild(apkLink);
    } else {
        var apkQr = ui.dom("div", {
            display: "flex", flexDirection: "column", alignItems: "center",
            marginTop: "20px", gap: "8px"
        });
        var apkCaption = ui.setText(ui.dom("div", {
            fontSize: "0.8rem", color: color.onSurfaceVar, textAlign: "center"
        }), t("downloadAndroidQr"));
        var apkBox = ui.dom("div", {
            width: "128px", height: "128px", padding: "8px", boxSizing: "border-box",
            background: "#fff", borderRadius: "8px"
        });
        apkQr.appendChild(apkCaption);
        apkQr.appendChild(apkBox);
        card.appendChild(apkQr);
        tanka.loadQrLib().then(function () { renderApkQr(apkBox, apkUrl); });
    }

    // Surface a failed WeChat / GitHub callback exchange (flagged by auth.js before
    // it re-rendered the login view).
    if (auth.takeWeChatError()) { ui.setText(status, t("wechatFailed")); }
    if (auth.takeGitHubError()) { ui.setText(status, t("githubFailed")); }

    // Height-aware fallback: once mounted, if the labeled stack overflows the
    // viewport, latch compact and re-render as an icon row. Only ever flips
    // labeled -> compact (compact always fits), so it converges without looping.
    if (!compact) {
        requestAnimationFrame(function () {
            if (compactFit) { return; }
            if (document.documentElement.scrollHeight > window.innerHeight + 1) {
                compactFit = true;
                app.render();
            }
        });
    }

    // Re-evaluate on resize: drop the latch so a now-taller window restores the
    // labels (the check above re-trips it if it still overflows). Debounced, and
    // only while the login view is up.
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
            if (net.getToken()) { return; }   // not on the login view
            if (window.innerWidth === lastWidth) { return; }   // height-only (soft keyboard): ignore
            lastWidth = window.innerWidth;
            if (pending) { clearTimeout(pending); }
            pending = setTimeout(function () {
                pending = null;
                compactFit = false;
                app.render();
            }, 150);
        });
    }

    return card;
};

//----------------------------------------------------------------------------

exports.buildLogin = buildLogin;

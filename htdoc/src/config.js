
//----------------------------------------------------------------------------
// App-wide configuration, the Material 3 color scheme, the shared mutable
// `state`, and the small presentation helpers (language / font / layout) that
// every view reads. Kept dependency-light (only net, for the backend presets)
// so it sits at the bottom of the module graph and everything else can require
// it without cycles.
//----------------------------------------------------------------------------

const net = require("./net");

var DEFAULT_MODEL = "gemini-2.5-flash";

// Package version, injected at build by webpack's DefinePlugin (see
// webpack.config.js); shown in the About dialog.
var APP_VERSION = (typeof __APP_VERSION__ === "string") ? __APP_VERSION__ : "";

// Remembers the last provider the user picked, so the selector restores it on
// the next visit (the app persists the selected model the same way).
var PROVIDER_KEY = "mirobody-provider";

// Remembers the chosen language; it rides on each agent request (the `language`
// field) so replies come back in that language, as the app does.
var LANGUAGE_KEY = "mirobody-language";

// The ten languages the app offers (code -> native label). The first eight
// match the app's LanguageDialog; Arabic and Hebrew are web-only and read
// right-to-left (see RTL_LANGS / applyDirection). Native labels are built from
// code points via fromCharCode so this source stays ASCII regardless of how the
// server labels the script's charset. Labels (in order): Chinese, Japanese,
// Korean, English, French, German, Russian, Spanish, Arabic, Hebrew.
var cc = String.fromCharCode;
var LANGUAGES = [
    ["zh", cc(0x4e2d, 0x6587)],
    ["ja", cc(0x65e5, 0x672c, 0x8a9e)],
    ["ko", cc(0xd55c, 0xad6d, 0xc5b4)],
    ["en", "English"],
    ["fr", "Fran" + cc(0xe7) + "ais"],
    ["de", "Deutsch"],
    ["ru", cc(0x0420, 0x0443, 0x0441, 0x0441, 0x043a, 0x0438, 0x0439)],
    ["es", "Espa" + cc(0xf1) + "ol"],
    ["ar", cc(0x0627, 0x0644, 0x0639, 0x0631, 0x0628, 0x064a, 0x0629)],
    ["he", cc(0x05e2, 0x05d1, 0x05e8, 0x05d9, 0x05ea)]
];

// Right-to-left languages: the document's `dir` is flipped for these so the
// layout mirrors (see applyDirection). Direction-sensitive styles use CSS
// logical properties / flexbox so they follow `dir` automatically.
var RTL_LANGS = { ar: 1, he: 1 };
function isRtl(code) { return !!RTL_LANGS[code]; }

// Mirror the whole UI for RTL languages by setting <html dir>. render() only
// clears #app, not the root, so this persists across re-renders; call it on
// startup and whenever the language changes.
function applyDirection(code) {
    document.documentElement.dir = isRtl(code) ? "rtl" : "ltr";
};

// Initial language: the browser's language if it's one we offer, else English
// (mirrors SettingsStore.defaultLanguage in the app).
function defaultLanguage() {
    var sys = (navigator.language || "en").slice(0, 2).toLowerCase();
    var supported = { zh: 1, ja: 1, ko: 1, en: 1, fr: 1, de: 1, ru: 1, es: 1, ar: 1, he: 1 };
    return supported[sys] ? sys : "en";
};

function languageLabel(code) {
    for (var i = 0; i < LANGUAGES.length; i ++) {
        if (LANGUAGES[i][0] === code) { return LANGUAGES[i][1]; }
    }
    return code;
};

// Remembers the UI font-size offset, added to the 16px base. The five tiers
// (offset -> i18n label key) match the app's FontSizeDialog.
var FONT_KEY = "mirobody-font-offset";
var FONT_TIERS = [
    [-4, "fontSmaller"],
    [-2, "fontSmall"],
    [0,  "fontNormal"],
    [2,  "fontLarge"],
    [4,  "fontLarger"]
];

// Scale the whole UI by setting the root font size; every rem-based size follows.
function applyFontScale(offset) {
    document.documentElement.style.fontSize = (16 + offset) + "px";
};

function fontTierLabel(offset) {
    for (var i = 0; i < FONT_TIERS.length; i ++) {
        if (FONT_TIERS[i][0] === offset) { return FONT_TIERS[i][1]; }
    }
    return "fontNormal";
};

// Backend base-URL suggestions for the Backend dialog: this server's own base
// (origin plus any URI prefix the app is mounted under) plus the hosted Mirobody
// environments, mirroring the app's BASE_URL_PRESETS.
var BASE_URL_PRESETS = [
    net.appBase() || window.location.origin,
    "https://test.mirobody.ai",
    "https://gray.mirobody.ai",
    "https://mirobody.ai"
];

// "Theta Health" light scheme: navy primary on warm cream surfaces, black serif
// brand. A deliberate departure from the app's M3 slate-blue/off-white so the web
// client reads as the Theta Health brand (see the login design).
var color = {
    primary      : "#1e3a6b",                  // deep navy: primary buttons, links, logo
    onPrimary    : "#ffffff",
    brand        : "#1e3a6b",                  // used as the user-bubble tint
    background   : "#f2efe9",                  // warm cream page background
    surfaceLow   : "#faf7f1",                  // field / card fill (slightly lighter than bg)
    onSurface    : "#1a1c1e",                  // near-black text + serif brand title
    onSurfaceVar : "#52565c",                  // secondary text (subtitle, hints)
    outline      : "#74787c",
    outlineVar   : "#ddd6c9",                  // warm hairlines / idle field borders
    userBubble   : "rgba(30, 58, 107, 0.10)",  // navy @ 10%
    error        : "#ba1a1a"
};

// Serif stack for the brand title (system fonts only — a strict font-src 'self'
// CSP blocks web fonts, so no Playfair/Didone download).
var serifFamily = '"Iowan Old Style", "Palatino Linotype", Palatino, Georgia, "Times New Roman", serif';

var state = {
    email     : "",
    model     : DEFAULT_MODEL,
    messages  : [],    // {role, content}
    streaming : false,
    providers : [],    // [{code, name}] from /api/providers; name is "Agent/provider"
    language  : localStorage.getItem(LANGUAGE_KEY) || defaultLanguage(),
    fontOffset : parseInt(localStorage.getItem(FONT_KEY), 10) || 0,
    provider  : localStorage.getItem(PROVIDER_KEY) || "",  // last selection ("Agent/provider")
    // The server-side conversation (thread) id of the current chat, learned from
    // the chat stream's "conversation" event and echoed back to continue the same
    // thread; also what the Share action shares. "" => a fresh thread.
    currentConversationId : "",
    // True when viewing a conversation shared *to* the user (read-only): the
    // composer is hidden and the thread can't be continued.
    readOnly  : false,
    // The chat "currently for" subject: "" / "0" => the user themselves; otherwise
    // a care-circle member's user id (someone who shared their health data), sent
    // as `subject` so the AI's family_health tool defaults to them.
    currentSubjectId : ""
};

// Two layouts: a centered card on wide screens, a full-width column on phones.
// The breakpoint is read at render time and the app re-renders when it's
// crossed (resize / rotation), so the layout always matches the viewport.
var MOBILE_QUERY = "(max-width: 600px)";
function isMobile() {
    return !!(window.matchMedia && window.matchMedia(MOBILE_QUERY).matches);
};

// True on Android (any browser). Used to decide how to offer the APK: a direct
// download link on Android, a scan-me QR code everywhere else (PC).
function isAndroid() {
    return /android/i.test(navigator.userAgent || "");
};

//----------------------------------------------------------------------------

exports.DEFAULT_MODEL    = DEFAULT_MODEL;
exports.APP_VERSION      = APP_VERSION;
exports.PROVIDER_KEY     = PROVIDER_KEY;
exports.LANGUAGE_KEY     = LANGUAGE_KEY;
exports.FONT_KEY         = FONT_KEY;
exports.LANGUAGES        = LANGUAGES;
exports.RTL_LANGS        = RTL_LANGS;
exports.FONT_TIERS       = FONT_TIERS;
exports.BASE_URL_PRESETS = BASE_URL_PRESETS;
exports.color            = color;
exports.serifFamily      = serifFamily;
exports.MOBILE_QUERY     = MOBILE_QUERY;
exports.state            = state;

exports.isRtl          = isRtl;
exports.applyDirection = applyDirection;
exports.defaultLanguage = defaultLanguage;
exports.languageLabel  = languageLabel;
exports.applyFontScale = applyFontScale;
exports.fontTierLabel  = fontTierLabel;
exports.isMobile       = isMobile;
exports.isAndroid      = isAndroid;

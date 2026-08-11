
//----------------------------------------------------------------------------
// Slash commands -- text the composer answers by itself, without a turn.
//
// A port of harmony's core/SlashCommand.ets and android's ui/chat/SlashCommand.kt,
// matched word for word: the three clients offer the same three commands, decide
// what a draft IS the same way, and open the same palette on a lone "/".
//
// `/help` renders the built-in guide (static/help/), which is written as a model
// reply on purpose: it is the user's documentation AND a live check of every
// construct markdown.js claims to render. A formatting regression shows up the
// first time anyone types /help, without a model and without a turn.
//
// A command NEVER reaches a model. It produces a message flagged `local`, which
// db.saveMessages drops when it mirrors the thread and the on-device bridge drops
// when it builds its history -- so a guide several thousand characters long costs
// nothing in context on the next turn and leaves no stored row. Same reasoning
// that keeps an ECharts option out of a message's text.
//
// harmony and android also carry an undocumented `/probe`, a developer screen for
// their in-process native core. This client has no native core -- it talks HTTP --
// so the word is left unbound rather than pointed at something else, which would
// break the one vocabulary the other two share.
//----------------------------------------------------------------------------

const i18n = require("./i18n");
const net  = require("./net");

//----------------------------------------------------------------------------

// The built-in guide. Documented -- it is listed in its own command table.
var HELP = "/help";

// Start a fresh conversation -- the drawer's "New chat", by keyboard.
var NEW = "/new";

// Toggle incognito -- the drawer's toggle, by keyboard.
var INCOGNITO = "/incognito";

// The commands the palette OFFERS, in the order it lists them. `desc` is an i18n
// key for the one-line description shown beside the name.
//
// `/help` leads because the palette is the discovery mechanism: whoever opened it
// by typing a slash is exploring, and the first row should be the one that
// explains the rest.
//
// Nothing here REPLACES a menu item. Every one of these is also a control in the
// drawer, and stays one: a command line is a shortcut for people who like them,
// not a syntax to memorize because the buttons went away.
var MENU = [
    { name: HELP,      desc: "slashHelpDesc" },
    { name: NEW,       desc: "slashNewDesc" },
    { name: INCOGNITO, desc: "slashIncognitoDesc" }
];

//----------------------------------------------------------------------------

// The palette's rows for a draft, or [] when the draft is not a command being
// typed.
//
// Opens on a leading "/" and narrows by prefix as the user types, so "/n" shows
// one row. It closes as soon as a space appears: past that the user is writing a
// sentence that happens to start with a slash, not choosing a command.
function suggest(draft) {
    var text = String(draft || "").replace(/^\s+/, "");
    if (text.charAt(0) !== "/" || text.indexOf(" ") >= 0 || text.indexOf("\n") >= 0) {
        return [];
    }
    var q = text.toLowerCase();
    var out = [];
    for (var i = 0; i < MENU.length; i ++) {
        if (MENU[i].name.indexOf(q) === 0) { out.push(MENU[i]); }
    }
    return out;
};

// The command this draft IS, or "".
//
// Matched on the WHOLE trimmed message, so a line that merely mentions "/help"
// mid-sentence still goes to the model -- a user asking "what does /help do" is
// asking a question, not running one.
function match(text) {
    var q = String(text || "").trim().toLowerCase();
    for (var i = 0; i < MENU.length; i ++) {
        if (MENU[i].name === q) { return MENU[i].name; }
    }
    return "";
};

//----------------------------------------------------------------------------

// The guide's markdown, in the app's language, as a promise of the text ("" when
// it could not be read).
//
// zh or en, like the other clients -- the guide is prose about this app, not a UI
// string table, so it is written rather than translated ten ways. It ships in the
// build (static/help/, copied to the doc root) and is fetched from the origin the
// client was served from, NOT from the configured backend: it is an asset of this
// build, like echarts.min.js, and a backend switch must not change it.
//
// Cached per language, because a user who types /help twice should not pay for it
// twice -- but only on success: caching a failed fetch would make one offline
// moment permanent for the session.
var cache = {};

function help() {
    var name = i18n.getLang().indexOf("zh") === 0 ? "help-zh.md" : "help-en.md";
    if (!cache[name]) {
        cache[name] = fetch(net.appBase() + "/help/" + name)
            .then(function (res) { return res.ok ? res.text() : ""; })
            .catch(function () { return ""; })
            .then(function (text) {
                if (!text) { delete cache[name]; }   // retry on the next /help
                return text;
            });
    }
    return cache[name];
};

//----------------------------------------------------------------------------

exports.HELP      = HELP;
exports.NEW       = NEW;
exports.INCOGNITO = INCOGNITO;
exports.MENU      = MENU;
exports.suggest   = suggest;
exports.match     = match;
exports.help      = help;

//----------------------------------------------------------------------------

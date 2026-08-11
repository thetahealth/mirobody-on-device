// Slash commands -- text the composer answers by itself, without a turn.
//
// A port of htdoc/src/slash.js (itself a port of harmony's core/SlashCommand.ets
// and android's ui/chat/SlashCommand.kt), matched word for word: the clients offer
// the same three commands, decide what a draft IS the same way, and open the same
// palette on a lone "/".
//
// `/help` renders the built-in guide, which is written as a model reply on purpose:
// it is the user's documentation AND a live check of every construct the Markdown
// renderer claims to support. A formatting regression shows up the first time
// anyone types /help, without a model, a network, or a backend.
//
// A command NEVER reaches a model. It produces a message the C++ side flags
// `local`, which ChatModel::snapshot() drops when the conversation is mirrored to
// disk and ChatModel::context() drops when a turn's history is built -- so a guide
// several thousand characters long costs nothing in context on the next turn and
// leaves no stored row.
//
// harmony and android also carry an undocumented `/probe`, a developer screen for
// their in-process native core. This client has none, so the word is left unbound
// rather than pointed at something else, which would break the one vocabulary the
// other clients share.
//
// `.pragma library` makes this a shared, stateless module (the guide itself is read
// from the Qt resource by AppController::helpDocument(), not from here -- QML JS has
// no file access).

.pragma library

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
    for (var i = 0; i < MENU.length; ++i) {
        if (MENU[i].name.indexOf(q) === 0) { out.push(MENU[i]); }
    }
    return out;
}

// The command this draft IS, or "".
//
// Matched on the WHOLE trimmed message, so a line that merely mentions "/help"
// mid-sentence still goes to the model -- a user asking "what does /help do" is
// asking a question, not running one.
function match(text) {
    var q = String(text || "").trim().toLowerCase();
    for (var i = 0; i < MENU.length; ++i) {
        if (MENU[i].name === q) { return MENU[i].name; }
    }
    return "";
}
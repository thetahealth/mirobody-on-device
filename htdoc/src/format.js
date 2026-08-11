
//----------------------------------------------------------------------------
// Timestamp formatters: a clock time for the chat log, and the history drawer's
// relative-then-absolute stamp.
//----------------------------------------------------------------------------

const i18n = require("./i18n");

function p(n) { return (n < 10 ? "0" : "") + n; }

// Render a local Date as "HH:MM" when it falls on today, else "YYYY-MM-DD
// HH:MM". Today's exchanges only need the time; older ones carry the date.
function renderLocal(d) {
    var time = p(d.getHours()) + ":" + p(d.getMinutes());
    var now = new Date();
    if (d.getFullYear() === now.getFullYear() &&
        d.getMonth() === now.getMonth() &&
        d.getDate() === now.getDate()) {
        return time;
    }
    return d.getFullYear() + "-" + p(d.getMonth() + 1) + "-" + p(d.getDate()) + " " + time;
}

// A backend timestamp as a local Date, or null. The modern backends send a unix-
// milliseconds integer; the legacy backend sends an ISO-8601 string (sometimes
// naive/offset-less UTC, so a missing timezone is treated as UTC).
function toDate(raw) {
    if (!raw) { return null; }
    var d;
    // Numeric value (or numeric string) => unix milliseconds.
    if (typeof raw === "number" || /^\d+$/.test(String(raw).trim())) {
        d = new Date(Number(raw));
        return isNaN(d.getTime()) ? null : d;
    }
    // SQLite returns "YYYY-MM-DD HH:MM:SS" (naive UTC); PostgreSQL returns it
    // with a "+00" / "+0000" offset. Normalize to ISO: space -> "T", pad the
    // offset to "+HH:MM", and treat a missing zone as UTC.
    var s = String(raw).trim().replace(" ", "T");
    s = s.replace(/([+\-]\d\d)(\d\d)$/, "$1:$2");   // +0000 -> +00:00
    s = s.replace(/([+\-]\d\d)$/, "$1:00");         // +00   -> +00:00
    if (!/[zZ]$|[+\-]\d\d:\d\d$/.test(s)) { s += "Z"; }
    d = new Date(s);
    return isNaN(d.getTime()) ? null : d;
}

//----------------------------------------------------------------------------

// The history drawer's stamp: relative for the recent past, absolute beyond a
// week. HarmonyOS's ladder, which Android also ports — a bare clock time on a
// three-week-old session reads as "today" at a glance, which is worse than no
// timestamp, and a bare date on something from ten minutes ago is equally
// unhelpful. "YYYY-MM-DD HH:MM" (what this was) is the first of those for every
// row that isn't from today.
//
// The two apps hand-roll the wording against their own plural resources, because
// Android's `DateUtils` and Harmony's equivalents read the SYSTEM language while
// the app carries its own setting. `Intl` takes the locale as an argument, so
// that objection does not apply here — and it brings each language's real plural
// rules with it (Russian needs one/few/many, Arabic six forms), which a string
// table of ours would have to reimplement to say the same thing. The ladder, the
// one-week cut and the year rule are the apps' to the step; only the wording
// comes from the platform.
//
// Everything is guarded: an engine without `Intl.RelativeTimeFormat` (or a
// locale it will not take) falls back to the absolute shape above rather than
// showing nothing.
var MINUTES_PER_WEEK = 7 * 24 * 60;

function relativeStamp(mins) {
    try {
        var rtf = new Intl.RelativeTimeFormat(i18n.getLang(), {
            numeric: "always",   // "1 day ago", not "yesterday" — as on the apps
            style   : "short"    // "2 hr ago", matching their own abbreviations
        });
        if (mins < 60) { return rtf.format(-mins, "minute"); }
        var hours = Math.floor(mins / 60);
        if (hours < 24) { return rtf.format(-hours, "hour"); }
        return rtf.format(-Math.floor(hours / 24), "day");
    } catch (e) {
        return "";
    }
}

// Past a week: a date. The year is carried only when it is not the current one —
// Harmony always omits it, which leaves "3/15" ambiguous once a conversation is
// more than a year old, so Android added the rule and this follows Android.
function absoluteStamp(d) {
    var options = { month: "short", day: "numeric" };
    if (d.getFullYear() !== new Date().getFullYear()) { options.year = "numeric"; }
    try {
        return new Intl.DateTimeFormat(i18n.getLang(), options).format(d);
    } catch (e) {
        return "";
    }
}

function formatStamp(raw) {
    var d = toDate(raw);
    if (!d) { return raw ? String(raw) : ""; }
    // Clamped at zero: a server clock a few seconds ahead of the browser would
    // otherwise render "in 1 minute".
    var mins = Math.floor((Date.now() - d.getTime()) / 60000);
    if (mins < 1) { return i18n.t("stampJustNow"); }
    var out = mins < MINUTES_PER_WEEK ? relativeStamp(mins) : absoluteStamp(d);
    return out || renderLocal(d);
};

//----------------------------------------------------------------------------

// Format a locally-generated epoch-ms timestamp (the chat messages' `ts`) in
// local time: the clock time, with the date only when it isn't today. A send
// time sits under the message it belongs to, in a thread the reader is already
// reading top to bottom, so "when today" is the whole question — the drawer's
// relative ladder answers a different one.
function formatLocalTime(ms) {
    var d = new Date(ms);
    if (isNaN(d.getTime())) { return ""; }
    return renderLocal(d);
};

//----------------------------------------------------------------------------

exports.formatStamp     = formatStamp;
exports.formatLocalTime = formatLocalTime;

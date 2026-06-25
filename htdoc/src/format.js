
//----------------------------------------------------------------------------
// Timestamp formatters shared by the chat log and the history drawer.
//----------------------------------------------------------------------------

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

// Format a backend timestamp in local time (date dropped for today; see
// renderLocal). The modern backends send a unix-milliseconds integer; the
// legacy backend sends an ISO-8601 string (sometimes naive/offset-less UTC, so
// a missing timezone is treated as UTC).
function formatTimestamp(raw) {
    if (!raw) { return ""; }
    // Numeric value (or numeric string) => unix milliseconds.
    if (typeof raw === "number" || /^\d+$/.test(String(raw).trim())) {
        return formatLocalTime(Number(raw));
    }
    // SQLite returns "YYYY-MM-DD HH:MM:SS" (naive UTC); PostgreSQL returns it
    // with a "+00" / "+0000" offset. Normalize to ISO: space -> "T", pad the
    // offset to "+HH:MM", and treat a missing zone as UTC.
    var s = String(raw).trim().replace(" ", "T");
    s = s.replace(/([+\-]\d\d)(\d\d)$/, "$1:$2");   // +0000 -> +00:00
    s = s.replace(/([+\-]\d\d)$/, "$1:00");         // +00   -> +00:00
    if (!/[zZ]$|[+\-]\d\d:\d\d$/.test(s)) { s += "Z"; }
    var d = new Date(s);
    if (isNaN(d.getTime())) { return raw; }
    return renderLocal(d);
};

// Format a locally-generated epoch-ms timestamp (the chat messages' `ts`) in
// local time, matching formatTimestamp's shape (date dropped for today).
function formatLocalTime(ms) {
    var d = new Date(ms);
    if (isNaN(d.getTime())) { return ""; }
    return renderLocal(d);
};

//----------------------------------------------------------------------------

exports.formatTimestamp = formatTimestamp;
exports.formatLocalTime = formatLocalTime;

// Local chat-history persistence backed by IndexedDB.
//
// The chat panel keeps the running conversation in memory (state.messages); this
// module mirrors it to IndexedDB so a page reload restores it instead of
// starting blank. Each conversation is stored under a key namespaced by the
// signed-in user id ("conversation:<sub>"), so two accounts on the same browser
// never read or write each other's history -- even if sign-out's clear() is
// missed (e.g. a silently expired session). With no signed-in user there is no
// key, so load/save/clear are no-ops.
//
// Everything is best-effort and promise-based: a browser without IndexedDB (or a
// private-mode failure) rejects, and callers fall back to the in-memory state.

var net = require("./net");

var DB_NAME       = "mirobody";
var DB_VERSION    = 1;
var STORE         = "kv";
var CONV_PREFIX   = "conversation:";
var CONVID_PREFIX = "conversation-id:";

// The IndexedDB key for the current user's conversation, or "" when nobody is
// signed in (caller treats "" as "nothing to load / save / clear").
function convKey() {
    var id = net.getUserId();
    return id ? CONV_PREFIX + id : "";
}

// Companion key holding the server thread id for that mirrored conversation, so a
// reload continues the same thread instead of forking a new one.
function convIdKey() {
    var id = net.getUserId();
    return id ? CONVID_PREFIX + id : "";
}

// Open (creating/upgrading) the database. Resolves with the IDBDatabase.
function open() {
    return new Promise(function (resolve, reject) {
        if (!window.indexedDB) {
            reject(new Error("IndexedDB unavailable"));
            return;
        }
        var req = window.indexedDB.open(DB_NAME, DB_VERSION);
        req.onupgradeneeded = function () {
            var db = req.result;
            if (!db.objectStoreNames.contains(STORE)) {
                db.createObjectStore(STORE);   // simple key -> value store
            }
        };
        req.onsuccess = function () { resolve(req.result); };
        req.onerror   = function () { reject(req.error || new Error("IndexedDB open failed")); };
    });
}

// Run `fn(store)` inside a transaction of `mode` and resolve once it commits.
// `fn` may return an IDBRequest whose result becomes the resolved value.
function withStore(mode, fn) {
    return open().then(function (db) {
        return new Promise(function (resolve, reject) {
            var tx = db.transaction(STORE, mode);
            var req = fn(tx.objectStore(STORE));
            tx.oncomplete = function () { resolve(req ? req.result : undefined); };
            tx.onerror    = function () { reject(tx.error); };
            tx.onabort    = function () { reject(tx.error); };
        });
    });
}

//----------------------------------------------------------------------------

// Load the stored conversation. Resolves to an array of {role, content}; an
// empty array when nothing is stored or IndexedDB is unavailable (never rejects,
// so callers can `.then` straight into render).
exports.loadMessages = function () {
    var key = convKey();
    if (!key) { return Promise.resolve([]); }
    return withStore("readonly", function (store) {
        return store.get(key);
    }).then(function (value) {
        return (value instanceof Array) ? value : [];
    }).catch(function () {
        return [];
    });
};

// Persist the conversation, replacing whatever was stored. Best-effort: a write
// failure is swallowed so it never interrupts the chat flow. The array is
// snapshotted to plain {role, content} objects up front so the value written is
// the state at call time, not whenever the async transaction happens to run.
//
// A `local` message (the built-in guide a slash command answered with -- see
// slash.js) is dropped here rather than at each call site: it is drawn in the
// thread and belongs to nothing else. Several KB of documentation is not part of
// the conversation the user had, and this is the one place that decides what a
// stored conversation is.
exports.saveMessages = function (messages) {
    var key = convKey();
    if (!key) { return Promise.resolve(); }
    var snapshot = (messages instanceof Array) ? messages.filter(function (m) {
        return !m.local;
    }).map(function (m) {
        return { role: m.role, content: m.content, ts: m.ts, cost: m.cost, provider: m.provider };
    }) : [];
    return withStore("readwrite", function (store) {
        return store.put(snapshot, key);
    }).catch(function () {});
};

// Load the stored server thread id for the mirrored conversation; "" when none.
exports.loadConversationId = function () {
    var key = convIdKey();
    if (!key) { return Promise.resolve(""); }
    return withStore("readonly", function (store) {
        return store.get(key);
    }).then(function (value) {
        return (typeof value === "string") ? value : "";
    }).catch(function () {
        return "";
    });
};

// Persist the server thread id alongside the mirrored messages. Best-effort.
exports.saveConversationId = function (id) {
    var key = convIdKey();
    if (!key) { return Promise.resolve(); }
    return withStore("readwrite", function (store) {
        return store.put(String(id || ""), key);
    }).catch(function () {});
};

// Drop the current user's stored conversation + its thread id (e.g. on sign-out).
// Best-effort. Must be called while the user is still signed in (before
// clearToken), since the keys are derived from the token.
exports.clear = function () {
    var mk = convKey(), ik = convIdKey();
    if (!mk && !ik) { return Promise.resolve(); }
    return withStore("readwrite", function (store) {
        if (mk) { store.delete(mk); }
        if (ik) { store.delete(ik); }
    }).catch(function () {});
};

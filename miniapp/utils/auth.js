// Per-account JWT storage — the Mini Program counterpart of the web client's
// net.js token helpers, adapted to WeChat's synchronous storage APIs
// (wx.getStorageSync/setStorageSync instead of localStorage).
//
// Each account's access token lives under TOKEN_PREFIX + <sub> (its JWT
// subject), and CURRENT_KEY names the active sub. Several accounts can coexist
// so the drawer can quick-switch between them instead of one token clobbering
// the next. LEGACY_KEY is the old single-token slot, migrated once on load.
var TOKEN_PREFIX = 'mirobody-x-token-';
var CURRENT_KEY  = 'mirobody-x-current';
var LEGACY_KEY   = 'mirobody-x-token';     // old single-token slot
var LEGACY_EXTRA = ['mirobody-x-refresh', 'mirobody-x-email']; // old side keys

// -- base64url / JWT decode -------------------------------------------------
// WeChat has no atob, so decode base64 ourselves. Padding ('=') is stripped and
// the 6-bit groups are re-packed into bytes; any non-alphabet char is skipped.
var B64_CHARS = 'ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/';
function base64ToBytes(input) {
  var str = String(input).replace(/=+$/, '');
  var bytes = [];
  var acc = 0, bits = 0;
  for (var i = 0; i < str.length; i++) {
    var idx = B64_CHARS.indexOf(str.charAt(i));
    if (idx < 0) { continue; }
    acc = (acc << 6) | idx;
    bits += 6;
    if (bits >= 8) {
      bits -= 8;
      bytes.push((acc >> bits) & 0xff);
    }
  }
  return bytes;
}

// Decode a UTF-8 byte array to a JS string (the JWT payload can carry non-ASCII,
// e.g. a unicode name); mirrors api.js utf8Decode.
function utf8Decode(bytes) {
  var out = '';
  var i = 0;
  var len = bytes.length;
  while (i < len) {
    var c = bytes[i++];
    if (c < 0x80) {
      out += String.fromCharCode(c);
    } else if (c < 0xE0) {
      out += String.fromCharCode(((c & 0x1F) << 6) | (bytes[i++] & 0x3F));
    } else if (c < 0xF0) {
      out += String.fromCharCode(
        ((c & 0x0F) << 12) | ((bytes[i++] & 0x3F) << 6) | (bytes[i++] & 0x3F));
    } else {
      var cp = ((c & 0x07) << 18) | ((bytes[i++] & 0x3F) << 12) |
               ((bytes[i++] & 0x3F) << 6) | (bytes[i++] & 0x3F);
      cp -= 0x10000;
      out += String.fromCharCode(0xD800 + (cp >> 10), 0xDC00 + (cp & 0x3FF));
    }
  }
  return out;
}

// Decode a JWT's payload (middle segment) into an object, or null when the
// token is missing/malformed. Base64url -> UTF-8 JSON, mirroring net.js.
function decodePayload(token) {
  if (!token) { return null; }
  var parts = String(token).split('.');
  if (parts.length < 2) { return null; }
  try {
    var b64 = parts[1].replace(/-/g, '+').replace(/_/g, '/');
    return JSON.parse(utf8Decode(base64ToBytes(b64)));
  } catch (e) {
    return null;
  }
}

// -- account slots ----------------------------------------------------------
function currentSub() { return wx.getStorageSync(CURRENT_KEY) || ''; }

function getToken() {
  var sub = currentSub();
  return sub ? (wx.getStorageSync(TOKEN_PREFIX + sub) || '') : '';
}

// Every stored account as { sub, email, current }, the current one first.
function listAccounts() {
  var out = [];
  var cur = currentSub();
  var keys = [];
  try { keys = (wx.getStorageInfoSync().keys) || []; } catch (e) { keys = []; }
  for (var i = 0; i < keys.length; i++) {
    var k = keys[i];
    if (!k || k.indexOf(TOKEN_PREFIX) !== 0) { continue; }
    var sub = k.slice(TOKEN_PREFIX.length);
    var p = decodePayload(wx.getStorageSync(k));
    out.push({ sub: sub, email: (p && p.email) ? String(p.email) : '', current: sub === cur });
  }
  out.sort(function (a, b) { return (b.current ? 1 : 0) - (a.current ? 1 : 0); });
  return out;
}

// Store a token under its own account slot and make it current. Dedupes by
// email: the server re-salts `sub` per issuance, so the same account re-logging
// in gets a new sub -- drop any existing slot with the same email first so
// there's one entry per account, not one per login (mirrors net.js setToken).
function setToken(token) {
  if (!token) { return; }
  var p = decodePayload(token);
  var sub = (p && p.sub) ? String(p.sub) : '';
  if (!sub) { return; }
  var email = (p && p.email) ? String(p.email) : '';
  if (email) {
    listAccounts().forEach(function (a) {
      if (a.sub !== sub && a.email === email) {
        wx.removeStorageSync(TOKEN_PREFIX + a.sub);
      }
    });
  }
  wx.setStorageSync(TOKEN_PREFIX + sub, token);
  wx.setStorageSync(CURRENT_KEY, sub);
}

// Make an already-stored account current. Returns false if its slot is gone.
function switchAccount(sub) {
  if (sub && wx.getStorageSync(TOKEN_PREFIX + sub)) {
    wx.setStorageSync(CURRENT_KEY, sub);
    return true;
  }
  return false;
}

// Sign out the CURRENT account: drop its slot, then fall back to another stored
// account if one exists (returns true, now current) or clear the pointer
// (returns false -> the caller shows the login screen). Mirrors net.js clearToken.
function signOut() {
  var sub = currentSub();
  if (sub) { wx.removeStorageSync(TOKEN_PREFIX + sub); }
  var rest = listAccounts();
  if (rest.length) {
    wx.setStorageSync(CURRENT_KEY, rest[0].sub);
    return true;
  }
  wx.removeStorageSync(CURRENT_KEY);
  return false;
}

// -- payload accessors ------------------------------------------------------
function decodeToken() { return decodePayload(getToken()); }

// A stable-per-session identifier for the signed-in user (the token's `sub`),
// used to namespace per-user local data so accounts never share it.
function getUserId() {
  var p = decodeToken();
  return p ? String(p.sub || p.email || '') : '';
}

// The signed-in user's email, from the token's `email` claim. '' when absent
// (e.g. a WeChat login whose token carries no email).
function getUserEmail() {
  var p = decodeToken();
  return (p && p.email) ? String(p.email) : '';
}

// -- migration + login integration ------------------------------------------
// One-time migration of the old single-token slot into the per-account scheme.
function migrateLegacy() {
  var legacy = wx.getStorageSync(LEGACY_KEY);
  if (legacy) {
    setToken(legacy);
    wx.removeStorageSync(LEGACY_KEY);
  }
  for (var i = 0; i < LEGACY_EXTRA.length; i++) {
    if (wx.getStorageSync(LEGACY_EXTRA[i])) { wx.removeStorageSync(LEGACY_EXTRA[i]); }
  }
}
migrateLegacy();

// Persist the auth envelope returned by a verify endpoint
// ({ access_token, ... }) under its own account slot and make it current.
// Adding a second account never drops the others (see setToken's per-sub keys),
// so "Add account" just runs a fresh login. `email` is accepted for call-site
// compatibility but ignored -- the email is read from the JWT (getUserEmail).
function setAuth(data, email) {
  if (data && data.access_token) {
    setToken(data.access_token);
  }
}

module.exports = {
  getToken: getToken,
  getUserId: getUserId,
  getUserEmail: getUserEmail,
  listAccounts: listAccounts,
  setToken: setToken,
  switchAccount: switchAccount,
  signOut: signOut,
  setAuth: setAuth,
};

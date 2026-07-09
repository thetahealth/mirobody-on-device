// Network layer for the Mini Program — the wx.request counterpart of the web
// client's net.js. Two shapes:
//   - post()/get(): unwrap the { code, msg, data } envelope (code 0 == ok).
//   - stream():     read a Server-Sent-Events response chunk-by-chunk via
//                   wx.request({ enableChunked: true }) + onChunkReceived,
//                   the Mini Program equivalent of fetch + ReadableStream.
var config = require('../config.js');
var auth = require('./auth.js');

function apiUrl(uri) {
  return config.baseUrl ? config.baseUrl + uri : uri;
}

// Decode a UTF-8 ArrayBuffer to a JS string. We decode only whole SSE frames
// (see stream() byte-buffering), so a multi-byte character is never split
// across a decode call.
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

// ---------------------------------------------------------------------------

// POST a JSON body and resolve with the unwrapped `data`. Rejects with an
// Error carrying { code, data } on an envelope failure or transport error.
// Pass opts.noToken for login/verify endpoints that take no Authorization.
function post(uri, body, opts) {
  opts = opts || {};
  return new Promise(function (resolve, reject) {
    var header = { 'Content-Type': 'application/json' };
    if (!opts.noToken) {
      var token = auth.getToken();
      if (token) {
        header['Authorization'] = 'Bearer ' + token;
      }
    }

    wx.request({
      url: apiUrl(uri),
      method: 'POST',
      header: header,
      data: body || {},
      success: function (res) {
        if (res.statusCode === 401) { reject(makeErr('unauthorized', 401)); return; }
        if (res.statusCode !== 200) {
          reject(makeErr('HTTP ' + res.statusCode, res.statusCode));
          return;
        }
        var json = res.data;
        if (!json || json.code !== 0) {
          reject(makeErr((json && json.msg) || 'request failed',
                         json && json.code, json && json.data));
          return;
        }
        resolve(json.data);
      },
      fail: function (e) {
        reject(makeErr((e && e.errMsg) || 'network error'));
      },
    });
  });
}

function makeErr(msg, code, data) {
  var e = new Error(msg);
  e.code = code;
  e.data = data;
  return e;
}

// GET a URI and resolve with the unwrapped `data`, same envelope rules as post().
// A 401 rejects with an Error whose .code is 401 (message "unauthorized") so
// callers can route to re-login, mirroring stream()'s 401 handling.
function get(uri, opts) {
  opts = opts || {};
  return new Promise(function (resolve, reject) {
    var header = {};
    if (!opts.noToken) {
      var token = auth.getToken();
      if (token) { header['Authorization'] = 'Bearer ' + token; }
    }
    wx.request({
      url: apiUrl(uri),
      method: 'GET',
      header: header,
      success: function (res) {
        if (res.statusCode === 401) { reject(makeErr('unauthorized', 401)); return; }
        if (res.statusCode !== 200) { reject(makeErr('HTTP ' + res.statusCode, res.statusCode)); return; }
        var json = res.data;
        if (!json || json.code !== 0) {
          reject(makeErr((json && json.msg) || 'request failed', json && json.code, json && json.data));
          return;
        }
        resolve(json.data);
      },
      fail: function (e) { reject(makeErr((e && e.errMsg) || 'network error')); },
    });
  });
}

// ---------------------------------------------------------------------------

// Encode a JS string to an array of UTF-8 byte values (the encode counterpart
// of utf8Decode). Used to assemble a multipart body's text parts.
function utf8Encode(str) {
  var bytes = [];
  for (var i = 0; i < str.length; i++) {
    var c = str.charCodeAt(i);
    if (c < 0x80) {
      bytes.push(c);
    } else if (c < 0x800) {
      bytes.push(0xC0 | (c >> 6), 0x80 | (c & 0x3F));
    } else if (c >= 0xD800 && c <= 0xDBFF) {           // high surrogate
      var c2 = str.charCodeAt(++i);
      var cp = 0x10000 + ((c & 0x3FF) << 10) + (c2 & 0x3FF);
      bytes.push(0xF0 | (cp >> 18), 0x80 | ((cp >> 12) & 0x3F),
                 0x80 | ((cp >> 6) & 0x3F), 0x80 | (cp & 0x3F));
    } else {
      bytes.push(0xE0 | (c >> 12), 0x80 | ((c >> 6) & 0x3F), 0x80 | (c & 0x3F));
    }
  }
  return bytes;
}

// Assemble a multipart/form-data body as an ArrayBuffer. `fields` is a flat
// object of string values; `files` is [{ path, name }] read from disk via the
// FileSystemManager. All files use the field name "file" (repeated), matching
// the web client's FormData. WeChat has no FormData + streamed response, so we
// build the bytes ourselves and send them through wx.request (enableChunked).
function buildMultipart(boundary, fields, files) {
  var fs = wx.getFileSystemManager();
  var parts = [];   // each entry is a byte array or a Uint8Array
  function pushStr(s) { parts.push(utf8Encode(s)); }

  for (var k in fields) {
    if (!Object.prototype.hasOwnProperty.call(fields, k)) continue;
    var v = fields[k];
    if (v === undefined || v === null || v === '') continue;
    pushStr('--' + boundary + '\r\nContent-Disposition: form-data; name="' + k + '"\r\n\r\n' + v + '\r\n');
  }
  for (var i = 0; i < files.length; i++) {
    var f = files[i];
    pushStr('--' + boundary + '\r\nContent-Disposition: form-data; name="file"; filename="' +
            f.name + '"\r\nContent-Type: application/octet-stream\r\n\r\n');
    parts.push(new Uint8Array(fs.readFileSync(f.path)));   // raw file bytes
    pushStr('\r\n');
  }
  pushStr('--' + boundary + '--\r\n');

  var total = 0;
  for (var p = 0; p < parts.length; p++) { total += parts[p].length; }
  var out = new Uint8Array(total);
  var off = 0;
  for (var q = 0; q < parts.length; q++) {
    out.set(parts[q], off);   // set() accepts a plain array or a typed array
    off += parts[q].length;
  }
  return out.buffer;
}

// Wire an SSE RequestTask: byte-buffer chunks, dispatch complete "\n\n"-delimited
// frames to onMessage, and settle via onComplete / onError. Returns the four
// callbacks the wx.request options + task listeners need. Shared by stream()
// (JSON body) and streamForm() (multipart body).
//   onMessage(payload) — one SSE event's joined "data:" payload (a string).
//   onComplete()       — stream ended (or a "[DONE]" frame arrived).
//   onError(reason)    — transport error, non-200, or an "event: error" frame;
//                        "unauthorized" on a 401 so callers can re-login.
function sseConsumer(onMessage, onComplete, onError) {
  var finished = false;
  function done(ok, reason) {
    if (finished) return;
    finished = true;
    if (ok) { if (typeof onComplete === 'function') onComplete(); }
    else if (typeof onError === 'function') { onError(reason); }
  }

  // SSE frames are delimited by "\n\n" (0x0a 0x0a); accumulate raw bytes and
  // only decode complete frames so a multi-byte char is never split.
  var pending = [];

  function dispatch(frameBytes) {
    var block = utf8Decode(frameBytes);
    var lines = block.split('\n');
    var payload = '';
    var event = '';
    for (var i = 0; i < lines.length; i++) {
      if (lines[i].indexOf('data:') === 0) {
        payload += (payload ? '\n' : '') + lines[i].slice(5).replace(/^ /, '');
      } else if (lines[i].indexOf('event:') === 0) {
        event = lines[i].slice(6).trim();
      }
    }
    if (!payload) return;
    if (event === 'error') { done(false, payload); }
    else if (payload === '[DONE]') { done(true); }
    else if (typeof onMessage === 'function') { onMessage(payload); }
  }

  function drain() {
    var start = 0;
    for (var i = 1; i < pending.length && !finished; i++) {
      if (pending[i - 1] === 0x0a && pending[i] === 0x0a) {
        dispatch(pending.slice(start, i - 1));
        start = i + 1;
      }
    }
    if (start > 0) { pending = pending.slice(start); }
  }

  return {
    success: function () { done(true); },   // agent stream closes without "[DONE]"
    fail: function (e) { done(false, (e && e.errMsg) || 'network error'); },
    onHeaders: function (res) {
      if (res.statusCode === 401) { done(false, 'unauthorized'); }
      else if (res.statusCode !== 200) { done(false, 'HTTP ' + res.statusCode); }
    },
    onChunk: function (res) {
      if (finished) return;
      var arr = new Uint8Array(res.data);
      for (var i = 0; i < arr.length; i++) { pending.push(arr[i]); }
      drain();
    },
  };
}

function authHeader(extra) {
  var header = extra || {};
  var token = auth.getToken();
  if (token) { header['Authorization'] = 'Bearer ' + token; }
  return header;
}

// POST a JSON body and stream the Server-Sent-Events response. Returns the
// RequestTask (call .abort() to cancel).
function stream(uri, body, onMessage, onComplete, onError) {
  var h = sseConsumer(onMessage, onComplete, onError);
  var task = wx.request({
    url: apiUrl(uri),
    method: 'POST',
    header: authHeader({ 'Content-Type': 'application/json' }),
    data: body || {},
    enableChunked: true,
    success: h.success,
    fail: h.fail,
  });
  task.onHeadersReceived(h.onHeaders);
  task.onChunkReceived(h.onChunk);
  return task;
}

// POST a multipart/form-data body (string `fields` + `files` = [{ path, name }])
// and stream the SSE response, the analog of the web client's FormData + fetch
// path for attachment turns. Returns the RequestTask (call .abort() to cancel).
function streamForm(uri, fields, files, onMessage, onComplete, onError) {
  var boundary = '----mirobodyFormBoundary' + Date.now();
  var body;
  try {
    body = buildMultipart(boundary, fields, files || []);
  } catch (e) {
    if (typeof onError === 'function') { onError((e && e.message) || 'read file failed'); }
    return null;
  }
  var h = sseConsumer(onMessage, onComplete, onError);
  var task = wx.request({
    url: apiUrl(uri),
    method: 'POST',
    header: authHeader({ 'Content-Type': 'multipart/form-data; boundary=' + boundary }),
    data: body,
    enableChunked: true,
    success: h.success,
    fail: h.fail,
  });
  task.onHeadersReceived(h.onHeaders);
  task.onChunkReceived(h.onChunk);
  return task;
}

module.exports = {
  apiUrl: apiUrl,
  post: post,
  get: get,
  stream: stream,
  streamForm: streamForm,
};

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

// ---------------------------------------------------------------------------

// POST a JSON body and stream the Server-Sent-Events response. Returns the
// RequestTask (call .abort() to cancel). Callbacks:
//   onMessage(payload) — one SSE event's joined "data:" payload (a string).
//   onComplete()       — stream ended (or a "[DONE]" frame arrived).
//   onError(reason)    — transport error, non-200, or an "event: error" frame.
//                        Passes the literal "unauthorized" on a 401 so callers
//                        can route to re-login, mirroring net.stream().
function stream(uri, body, onMessage, onComplete, onError) {
  var header = { 'Content-Type': 'application/json' };
  var token = auth.getToken();
  if (token) {
    header['Authorization'] = 'Bearer ' + token;
  }

  var finished = false;
  function done(ok, reason) {
    if (finished) return;
    finished = true;
    if (ok) {
      if (typeof onComplete === 'function') onComplete();
    } else if (typeof onError === 'function') {
      onError(reason);
    }
  }

  // Byte-level buffer: SSE frames are delimited by "\n\n" (0x0a 0x0a). We
  // accumulate raw bytes and only decode complete frames, so a UTF-8 sequence
  // straddling two chunks is never decoded half-formed.
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
    if (event === 'error') {
      done(false, payload);
    } else if (payload === '[DONE]') {
      done(true);
    } else if (typeof onMessage === 'function') {
      onMessage(payload);
    }
  }

  function drain() {
    // Scan pending bytes for a "\n\n" boundary; dispatch each complete frame.
    var start = 0;
    for (var i = 1; i < pending.length && !finished; i++) {
      if (pending[i - 1] === 0x0a && pending[i] === 0x0a) {
        dispatch(pending.slice(start, i - 1));
        start = i + 1;
      }
    }
    if (start > 0) {
      pending = pending.slice(start);
    }
  }

  var task = wx.request({
    url: apiUrl(uri),
    method: 'POST',
    header: header,
    data: body || {},
    enableChunked: true,
    success: function () {
      // Whole response delivered; the agent stream ends without a "[DONE]"
      // frame, so completion of the request is the completion signal.
      done(true);
    },
    fail: function (e) {
      done(false, (e && e.errMsg) || 'network error');
    },
  });

  task.onHeadersReceived(function (res) {
    if (res.statusCode === 401) {
      done(false, 'unauthorized');
    } else if (res.statusCode !== 200) {
      done(false, 'HTTP ' + res.statusCode);
    }
  });

  task.onChunkReceived(function (res) {
    if (finished) return;
    var arr = new Uint8Array(res.data);
    for (var i = 0; i < arr.length; i++) {
      pending.push(arr[i]);
    }
    drain();
  });

  return task;
}

module.exports = {
  apiUrl: apiUrl,
  post: post,
  stream: stream,
};

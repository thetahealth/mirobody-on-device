// Chat page — the Mini Program counterpart of the web client's chat view.
// Lists agents/providers from POST /api/providers, then streams answers from
// POST /api/chat (agent mode: body { agent, provider, question, language };
// SSE events { type: "reply"|"thinking"|"costStatistics"|"error", ... }).
var config = require('../../config.js');
var api = require('../../utils/api.js');
var auth = require('../../utils/auth.js');

Page({
  data: {
    messages: [],        // [{ role: 'user'|'assistant', content, thinking }]
    input: '',
    providers: [],       // [{ code: 'agent/provider', name }]
    providerIndex: 0,
    streaming: false,
    scrollInto: '',      // id of the element to scroll into view
  },

  task: null,            // in-flight stream RequestTask

  onLoad: function () {
    if (!auth.getToken()) {
      wx.redirectTo({ url: '/pages/login/login' });
      return;
    }
    this.loadProviders();
  },

  onUnload: function () {
    if (this.task) {
      this.task.abort();
      this.task = null;
    }
  },

  // POST /api/providers — public (no auth). Populates the picker.
  loadProviders: function () {
    var self = this;
    api.post('/api/providers', null, { noToken: true })
      .then(function (data) {
        self.setData({ providers: (data instanceof Array) ? data : [] });
      })
      .catch(function () { /* leave the picker empty; sending is guarded */ });
  },

  onInput: function (e) {
    this.setData({ input: e.detail.value });
  },

  onProviderChange: function (e) {
    this.setData({ providerIndex: Number(e.detail.value) || 0 });
  },

  onSignOut: function () {
    auth.clearAuth();
    wx.reLaunch({ url: '/pages/login/login' });
  },

  onSend: function () {
    var text = (this.data.input || '').trim();
    if (!text || this.data.streaming) return;

    var providers = this.data.providers;
    if (!providers.length) {
      wx.showToast({ title: '正在加载可用模型…', icon: 'none' });
      return;
    }
    var code = providers[this.data.providerIndex].code || '';
    var slash = code.indexOf('/');
    var agent = slash >= 0 ? code.slice(0, slash) : code;
    var provider = slash >= 0 ? code.slice(slash + 1) : '';

    // Append the user turn and an empty assistant turn we stream into.
    var msgs = this.data.messages.concat([
      { role: 'user', content: text, thinking: '' },
      { role: 'assistant', content: '', thinking: '' },
    ]);
    var aIdx = msgs.length - 1;

    var self = this;
    this.setData({
      messages: msgs,
      input: '',
      streaming: true,
      scrollInto: 'msg-' + aIdx,
    });

    var acc = '';
    var think = '';

    this.task = api.stream(
      '/api/chat',
      { agent: agent, provider: provider, question: text, language: config.language },
      function (chunk) { // onMessage
        var ev = parseAgentChunk(chunk);
        if (typeof ev.reply === 'string') {
          acc += ev.reply;
          self.patch(aIdx, { content: acc });
        } else if (typeof ev.thinking === 'string') {
          think += ev.thinking;
          self.patch(aIdx, { thinking: think });
        } else if (ev.error) {
          self.patch(aIdx, { content: (acc || '') + '\n[出错] ' + ev.error });
        }
      },
      function () { // onComplete
        self.task = null;
        self.setData({ streaming: false });
      },
      function (reason) { // onError
        self.task = null;
        if (reason === 'unauthorized') {
          self.onSignOut();
          return;
        }
        self.patch(aIdx, { content: acc || ('[请求失败] ' + reason) });
        self.setData({ streaming: false });
      }
    );
  },

  // Update one field of one message and keep the view scrolled to it.
  patch: function (index, fields) {
    var data = {};
    for (var k in fields) {
      if (Object.prototype.hasOwnProperty.call(fields, k)) {
        data['messages[' + index + '].' + k] = fields[k];
      }
    }
    data.scrollInto = 'msg-' + index;
    this.setData(data);
  },
});

// Map an agent SSE event to { reply | thinking | cost | error }, matching the
// web client's parseAgentChunk in htdoc/src/index.js.
function parseAgentChunk(chunk) {
  var json = null;
  try {
    json = JSON.parse(chunk);
  } catch (err) {
    return {};
  }
  if (json.type === 'reply')    return { reply: json.content || '' };
  if (json.type === 'thinking') return { thinking: json.content || '' };
  if (json.type === 'costStatistics' && json.cost) return { cost: json.cost };
  if (json.type === 'error')    return { error: json.content || 'error' };
  return {};
}

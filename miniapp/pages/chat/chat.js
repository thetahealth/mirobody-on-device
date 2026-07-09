// Chat page — the Mini Program counterpart of the web client's chat view.
// Lists agents/providers from POST /api/providers, then streams answers from
// POST /api/chat. The composer mirrors the web client's (htdoc/src/chat.js):
// one rounded bar with a multi-line textarea, an attach button + attachment
// preview chips, the provider picker, an optional care-circle "subject" picker,
// and a circular send button that becomes a stop button while streaming.
//
// Chat body fields (JSON, or multipart when files ride along): agent, provider,
// question, language, conversation_id, subject. SSE events: reply / thinking /
// conversation (thread id) / upload / transcript / costStatistics / error.
//
// The hamburger opens a left nav drawer — a New chat / Incognito button row,
// past sessions (resume/delete), WeChat-steps sync, care circle, and sign out.
// Language moved to the top-bar settings gear (mirrors the web client, whose
// gear carries the app settings on both the login and chat screens).
var config = require('../../config.js');
var api = require('../../utils/api.js');
var auth = require('../../utils/auth.js');
var werun = require('../../utils/werun.js');

// Conversation languages offered by the menu — the value drives the `language`
// field sent to /api/chat (the UI copy itself stays Chinese).
var LANGS = [
  { code: 'zh-CN', name: '简体中文' },
  { code: 'en',    name: 'English' },
  { code: 'ja',    name: '日本語' },
  { code: 'ko',    name: '한국어' },
  { code: 'fr',    name: 'Français' },
  { code: 'de',    name: 'Deutsch' },
  { code: 'es',    name: 'Español' },
  { code: 'ru',    name: 'Русский' },
];

Page({
  data: {
    messages: [],        // [{ role: 'user'|'assistant', content, thinking }]
    input: '',
    providers: [],       // flattened [{ agent, provider }]; agent is "" for the default agent
    providerIndex: 0,
    streaming: false,
    scrollInto: '',      // id of the element to scroll into view

    // composer
    attachments: [],     // [{ id, path, name, size, sizeLabel, isImage }]
    barFocus: false,
    // Privacy mode: an ephemeral session that isn't persisted and carries
    // incognito:true on each request (the server then skips storage + memory).
    incognito: false,
    subjectNames: ['我'], // picker labels; index 0 = me
    subjectUsers: [],     // [{ member, name }] parallel to subjectNames[1..]
    subjectIndex: 0,
    showSubject: false,

    // drawer
    drawerOpen: false,
    settingsOpen: false, // top-bar settings menu (language)
    history: [],
    historyLoading: false,
    historyError: false,
    syncing: false,
    langNames: LANGS.map(function (l) { return l.name; }),
    langIndex: 0,
    langLabel: '',
  },

  task: null,            // in-flight stream RequestTask
  convId: '',            // durable conversation (thread) id, sent to continue a thread
  _incognitoSaved: null, // { messages, convId } stashed while incognito is active
  _attachSeq: 0,         // attachment id counter
  _historyLoaded: false,

  onLoad: function () {
    if (!auth.getToken()) {
      wx.redirectTo({ url: '/pages/login/login' });
      return;
    }
    var idx = 0;
    for (var i = 0; i < LANGS.length; i++) {
      if (LANGS[i].code === config.language) { idx = i; break; }
    }
    this.setData({ langIndex: idx, langLabel: LANGS[idx].name });
    this.loadProviders();
    this.loadSubjects();
  },

  onUnload: function () {
    if (this.task) { this.task.abort(); this.task = null; }
  },

  // -- providers ----------------------------------------------------------
  // POST /api/providers — JWT-guarded (it exposes the configured model names, so
  // it's only reachable after login; see src/chat/service.cpp). The server returns
  // groups [{ agent, providers[] }]; flatten to one { agent, provider } per model
  // for the picker. Default agent's group has an empty agent, so its models list bare.
  loadProviders: function () {
    var self = this;
    api.post('/api/providers', null)
      .then(function (data) {
        var groups = (data instanceof Array) ? data : [];
        var flat = [];
        for (var i = 0; i < groups.length; i++) {
          var g = groups[i] || {};
          var provs = (g.providers instanceof Array) ? g.providers : [];
          for (var j = 0; j < provs.length; j++) {
            flat.push({ agent: g.agent || '', provider: provs[j] });
          }
        }
        self.setData({ providers: flat });
      })
      .catch(function (err) {
        if (err && err.code === 401) { self.onSignOut(); return; }
        /* otherwise leave the picker empty; sending is guarded */
      });
  },

  // GET /api/circle/health-shared-with-me -> { users:[{ member, nickname, email }] }.
  // Picking a member sends `subject` so the agent's family_health tool defaults to
  // them ("how is Mom doing?"). Shown only when someone shared their data.
  loadSubjects: function () {
    var self = this;
    api.get('/api/circle/health-shared-with-me')
      .then(function (d) {
        var users = (d && d.users instanceof Array) ? d.users : [];
        var subs = [];
        for (var i = 0; i < users.length; i++) {
          var u = users[i];
          if (u.member == null) { continue; }
          subs.push({ member: String(u.member), name: u.nickname || u.email || ('#' + u.member) });
        }
        self.setData({
          subjectUsers: subs,
          subjectNames: ['我'].concat(subs.map(function (s) { return s.name; })),
          subjectIndex: 0,
          showSubject: subs.length > 0,
        });
      })
      .catch(function () { self.setData({ showSubject: false }); });
  },

  onInput: function (e) { this.setData({ input: e.detail.value }); },
  onBarFocus: function () { this.setData({ barFocus: true }); },
  onBarBlur: function () { this.setData({ barFocus: false }); },
  onProviderChange: function (e) { this.setData({ providerIndex: Number(e.detail.value) || 0 }); },
  onSubjectChange: function (e) { this.setData({ subjectIndex: Number(e.detail.value) || 0 }); },

  // -- attachments (wx.chooseMessageFile: images or files from chats) -----
  onAttach: function () {
    var self = this;
    wx.chooseMessageFile({
      count: 9,
      type: 'all',
      success: function (res) {
        var files = res.tempFiles || [];
        var add = files.map(function (f) {
          var isImage = f.type === 'image' || /\.(png|jpe?g|gif|webp|bmp)$/i.test(f.name || '');
          return {
            id: ++self._attachSeq,
            path: f.path,
            name: f.name || 'file',
            size: f.size || 0,
            sizeLabel: humanSize(f.size || 0),
            isImage: isImage,
          };
        });
        self.setData({ attachments: self.data.attachments.concat(add) });
      },
    });
  },

  onRemoveAttach: function (e) {
    var id = e.currentTarget.dataset.id;
    this.setData({
      attachments: this.data.attachments.filter(function (a) { return a.id !== id; }),
    });
  },

  // -- drawer -------------------------------------------------------------
  openDrawer: function () {
    this.setData({ drawerOpen: true });
    if (!this._historyLoaded) { this.loadHistory(); }
  },
  closeDrawer: function () { this.setData({ drawerOpen: false }); },

  onNewChat: function () {
    if (this.data.streaming) { this.closeDrawer(); return; }
    this.convId = '';
    this.setData({ messages: [], input: '', attachments: [], scrollInto: '', drawerOpen: false });
  },

  // Privacy mode: entering stashes the real conversation and starts a blank
  // ephemeral one; leaving restores the stash and drops the ephemeral turns. The
  // request carries incognito:true while on (see onSend), so the server persists
  // nothing. Never toggled mid-stream. Mirrors htdoc/src/chat.js toggleIncognito.
  onToggleIncognito: function () {
    if (this.data.streaming) { this.closeDrawer(); return; }
    if (!this.data.incognito) {
      this._incognitoSaved = { messages: this.data.messages, convId: this.convId };
      this.convId = '';
      this.setData({
        messages: [], input: '', attachments: [],
        incognito: true, drawerOpen: false, scrollInto: '',
      });
    } else {
      var saved = this._incognitoSaved || { messages: [], convId: '' };
      this.convId = saved.convId || '';
      this._incognitoSaved = null;
      var msgs = saved.messages || [];
      this.setData({
        messages: msgs, incognito: false, drawerOpen: false,
        scrollInto: msgs.length ? 'msg-' + (msgs.length - 1) : '',
      });
    }
  },

  // -- settings menu (top-bar gear) ---------------------------------------
  toggleSettings: function () { this.setData({ settingsOpen: !this.data.settingsOpen }); },
  closeSettings: function () { this.setData({ settingsOpen: false }); },

  onOpenCircle: function () {
    this.setData({ drawerOpen: false });
    wx.navigateTo({ url: '/pages/circle/circle' });
  },

  onLanguageChange: function (e) {
    var idx = Number(e.detail.value) || 0;
    var lang = LANGS[idx] || LANGS[0];
    config.setLanguage(lang.code);
    this.setData({ langIndex: idx, langLabel: lang.name, settingsOpen: false });
  },

  // -- history ------------------------------------------------------------
  loadHistory: function () {
    var self = this;
    this.setData({ historyLoading: true, historyError: false });
    api.get('/api/history?page=0&page_size=20')
      .then(function (data) {
        self._historyLoaded = true;
        var rows = (data && data.summaries instanceof Array) ? data.summaries : [];
        self.setData({
          historyLoading: false,
          history: rows.map(function (it) {
            return {
              session_id: it.session_id,
              title: it.summary || it.session_id || '未命名会话',
              time: formatTime(it.timestamp),
            };
          }),
        });
      })
      .catch(function (err) {
        if (err && err.code === 401) { self.onSignOut(); return; }
        self.setData({ historyLoading: false, historyError: true });
      });
  },

  onResume: function (e) {
    var self = this;
    var id = e.currentTarget.dataset.id;
    if (!id) { return; }
    api.get('/api/conversation?id=' + encodeURIComponent(id))
      .then(function (data) {
        var msgs = (data && data.messages instanceof Array) ? data.messages : [];
        var mapped = msgs.map(function (m) {
          return { role: m.role, content: m.content || '', thinking: '' };
        });
        // Continue this thread on the next turn.
        self.convId = String((data && data.id) || id);
        self.setData({
          messages: mapped,
          attachments: [],
          drawerOpen: false,
          scrollInto: mapped.length ? 'msg-' + (mapped.length - 1) : '',
        });
      })
      .catch(function (err) {
        if (err && err.code === 401) { self.onSignOut(); return; }
        wx.showToast({ title: '打开会话失败', icon: 'none' });
      });
  },

  onDeleteHistory: function (e) {
    var self = this;
    var id = e.currentTarget.dataset.id;
    if (!id) { return; }
    wx.showModal({
      title: '删除会话',
      content: '确定删除这条历史会话吗？',
      confirmColor: '#ba1a1a',
      success: function (r) {
        if (!r.confirm) { return; }
        var previous = self.data.history;
        self.setData({ history: previous.filter(function (it) { return it.session_id !== id; }) });
        api.post('/api/history/delete', { session_id: id })
          .catch(function (err) {
            if (err && err.code === 401) { self.onSignOut(); return; }
            self.setData({ history: previous });
            wx.showToast({ title: '删除失败', icon: 'none' });
          });
      },
    });
  },

  // -- WeChat steps -------------------------------------------------------
  onSyncSteps: function () {
    if (this.data.syncing) { return; }
    var self = this;
    this.setData({ syncing: true });
    werun.syncSteps()
      .then(function (r) {
        self.setData({ syncing: false });
        var posted = (r && r.posted) || 0;
        wx.showToast({ title: '已同步 ' + posted + ' 天步数', icon: 'none' });
      })
      .catch(function (err) {
        self.setData({ syncing: false });
        if (err && err.code === 401) { self.onSignOut(); return; }
        wx.showToast({ title: (err && err.message) || '同步失败', icon: 'none' });
      });
  },

  // -- account ------------------------------------------------------------
  onConfirmSignOut: function () {
    var self = this;
    wx.showModal({
      title: '退出登录',
      content: '确定要退出当前账号吗？',
      confirmColor: '#ba1a1a',
      success: function (r) { if (r.confirm) { self.onSignOut(); } },
    });
  },

  onSignOut: function () {
    auth.clearAuth();
    wx.reLaunch({ url: '/pages/login/login' });
  },

  // -- send / stream ------------------------------------------------------
  onSendOrStop: function () {
    if (this.data.streaming) {
      if (this.task) { this.task.abort(); this.task = null; }
      this.setData({ streaming: false });
      return;
    }
    this.onSend();
  },

  onSend: function () {
    var text = (this.data.input || '').trim();
    if (!text || this.data.streaming) { return; }

    var providers = this.data.providers;
    if (!providers.length) {
      wx.showToast({ title: '正在加载可用模型…', icon: 'none' });
      return;
    }
    var sel = providers[this.data.providerIndex] || {};
    var agent = sel.agent || '';
    var provider = sel.provider || '';

    // Only a positive-integer member handle is a valid subject; else "myself".
    var subjectId = '';
    if (this.data.subjectIndex > 0) {
      var su = this.data.subjectUsers[this.data.subjectIndex - 1];
      if (su && /^[0-9]+$/.test(su.member)) { subjectId = su.member; }
    }

    // Snapshot the picked files for this turn, then clear the chips.
    var turnFiles = this.data.attachments.map(function (a) { return { path: a.path, name: a.name }; });

    var msgs = this.data.messages.concat([
      { role: 'user', content: text, thinking: '' },
      { role: 'assistant', content: '', thinking: '' },
    ]);
    var aIdx = msgs.length - 1;

    var self = this;
    this.setData({
      messages: msgs,
      input: '',
      attachments: [],
      streaming: true,
      scrollInto: 'msg-' + aIdx,
    });

    var acc = '';
    var status = '';   // upload/transcript/thinking lines, shown above the reply

    var fields = {
      agent: agent,
      provider: provider,
      question: text,
      language: config.language,
      conversation_id: this.convId || '',
      subject: subjectId,
    };
    // Privacy mode: tell the server not to persist or add to memory. Sent only
    // when on -- a boolean in the JSON body, coerced to "true" in the multipart
    // path (buildMultipart), matching the web client.
    if (this.data.incognito) { fields.incognito = true; }

    function onMessage(chunk) {
      var ev = parseAgentChunk(chunk);
      if (ev.conversation) {
        if (ev.conversation.id) { self.convId = ev.conversation.id; }
      } else if (typeof ev.reply === 'string') {
        acc += ev.reply;
        self.patch(aIdx, { content: acc });
      } else if (typeof ev.thinking === 'string') {
        status += ev.thinking;
        self.patch(aIdx, { thinking: status });
      } else if (ev.upload) {
        status += (status ? '\n' : '') + '已上传：' + (ev.upload.filename || '');
        self.patch(aIdx, { thinking: status });
      } else if (ev.transcript && ev.transcript.phase === 'begin') {
        status += (status ? '\n' : '') + '正在读取：' + (ev.transcript.filename || '');
        self.patch(aIdx, { thinking: status });
      } else if (ev.error) {
        self.patch(aIdx, { content: (acc || '') + '\n[出错] ' + ev.error });
      }
    }
    function onComplete() {
      self.task = null;
      self.setData({ streaming: false });
    }
    function onError(reason) {
      self.task = null;
      if (reason === 'unauthorized') { self.onSignOut(); return; }
      self.patch(aIdx, { content: acc || ('[请求失败] ' + reason) });
      self.setData({ streaming: false });
    }

    // Attachments ride a multipart body (no FormData/streaming combo in WeChat,
    // so api.streamForm assembles the bytes itself); otherwise a light JSON body.
    if (turnFiles.length) {
      this.task = api.streamForm('/api/chat', fields, turnFiles, onMessage, onComplete, onError);
    } else {
      this.task = api.stream('/api/chat', fields, onMessage, onComplete, onError);
    }
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

// Map an agent SSE event to a small tagged object, matching the web client's
// parseAgentChunk (htdoc/src/chat.js).
function parseAgentChunk(chunk) {
  var json = null;
  try { json = JSON.parse(chunk); } catch (err) { return {}; }
  if (json.type === 'reply')    return { reply: json.content || '' };
  if (json.type === 'thinking') return { thinking: json.content || '' };
  if (json.type === 'costStatistics' && json.cost) return { cost: json.cost };
  if (json.type === 'upload')   return { upload: json.file || { filename: json.content || '' } };
  if (json.type === 'transcript') {
    return { transcript: { filename: json.content || '', phase: json.phase || '', extracted: !!json.extracted } };
  }
  if (json.type === 'error')    return { error: json.content || 'error' };
  if (json.type === 'conversation') {
    return { conversation: { id: (typeof json.content === 'string') ? json.content
                : (json.conversation_id != null ? String(json.conversation_id) : '') } };
  }
  return {};
}

// Best-effort timestamp -> "YYYY-MM-DD HH:MM". Accepts unix seconds, unix ms,
// or an ISO string; returns "" on anything unparseable.
function formatTime(ts) {
  if (!ts) { return ''; }
  var ms;
  if (typeof ts === 'number') {
    ms = ts < 1e12 ? ts * 1000 : ts;   // seconds vs milliseconds
  } else {
    ms = Date.parse(ts);
  }
  if (!ms || isNaN(ms)) { return ''; }
  var d = new Date(ms);
  function p(n) { return (n < 10 ? '0' : '') + n; }
  return d.getFullYear() + '-' + p(d.getMonth() + 1) + '-' + p(d.getDate()) +
         ' ' + p(d.getHours()) + ':' + p(d.getMinutes());
}

// Bytes -> a short human size for the attachment chip.
function humanSize(bytes) {
  if (!bytes) { return ''; }
  if (bytes < 1024) { return bytes + 'B'; }
  if (bytes < 1024 * 1024) { return Math.round(bytes / 1024) + 'KB'; }
  return (bytes / (1024 * 1024)).toFixed(1) + 'MB';
}

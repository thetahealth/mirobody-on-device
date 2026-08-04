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
// The top-left hamburger opens the app's ONLY menu, a left nav drawer — a New
// chat / Incognito button row, past sessions (resume/delete), WeChat-steps sync,
// care circle, the app-settings group (the shared <settings-rows> component:
// language / font size / backend), an account switcher, and sign out.
// The settings gear that used to sit at the top right is gone: folding it in here
// leaves one menu affordance instead of two, mirroring the web client.
var config = require('../../config.js');
var api = require('../../utils/api.js');
var auth = require('../../utils/auth.js');
var werun = require('../../utils/werun.js');

// Font-size offset -> the .page class that scales message text, matching the
// five tiers in the drawer's settings group (see components/settings-rows).
function fontClassFor(offset) {
  var map = { '-4': 'fs-smaller', '-2': 'fs-small', '0': '', '2': 'fs-large', '4': 'fs-larger' };
  return map[String(offset)] || '';
}

// Label for an account row: its email, else a short "#sub" fallback.
function acctLabel(a) {
  if (!a) { return ''; }
  return a.email ? a.email : ('#' + String(a.sub).slice(0, 6));
}

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

    // top bar / settings
    fontClass: '',       // font-size tier class chosen in the drawer's settings group

    // drawer
    drawerOpen: false,
    history: [],
    historyLoading: false,
    historyError: false,
    syncing: false,

    // account switcher (drawer footer)
    accountLabel: '',    // the current account's email (or #sub), muted
    otherAccounts: [],   // [{ sub, label }] the other signed-in accounts
    switcherOpen: false,
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
    this.setData({ fontClass: fontClassFor(config.fontOffset) });
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
    this.refreshAccounts();
    if (!this._historyLoaded) { this.loadHistory(); }
  },
  closeDrawer: function () { this.setData({ drawerOpen: false }); },

  // Populate the drawer's account switcher from the stored accounts: the
  // current one's label (muted) plus the others (tap to switch).
  refreshAccounts: function () {
    var accounts = auth.listAccounts();
    var current = null, others = [];
    accounts.forEach(function (a) {
      if (a.current) { current = a; }
      else { others.push({ sub: a.sub, label: acctLabel(a) }); }
    });
    this.setData({ accountLabel: acctLabel(current), otherAccounts: others });
  },

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

  // -- app-settings events (from the shared <settings-rows> component) -----
  // Language selection lives entirely in the component (config.language is read
  // per request in onSend), so only font + backend need a host reaction.
  onFontChange: function (e) {
    this.setData({ fontClass: (e.detail && e.detail.fontClass) || '' });
  },
  // A backend switch takes effect on the next request; refetch the auth-gated
  // lists so what's on screen matches the new backend right away.
  onBackendChange: function () {
    this._historyLoaded = false;
    this.loadProviders();
    this.loadSubjects();
    if (this.data.drawerOpen) { this.loadHistory(); }
  },

  onOpenCircle: function () {
    this.setData({ drawerOpen: false });
    wx.navigateTo({ url: '/pages/circle/circle' });
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

  // -- account switcher ---------------------------------------------------
  toggleSwitcher: function () { this.setData({ switcherOpen: !this.data.switcherOpen }); },

  // Switch to another already-stored account, then relaunch so every view
  // reloads fresh under the new token (no previous account's state leaks).
  onSwitchAccount: function (e) {
    var sub = e.currentTarget.dataset.sub;
    if (sub && auth.switchAccount(sub)) {
      wx.reLaunch({ url: '/pages/chat/chat' });
    }
  },

  // Add another account: open the login page over the current session. The
  // stored accounts aren't dropped (auth.setToken keys per JWT sub), so a
  // successful login just adds a slot; the native back button cancels.
  onAddAccount: function () {
    this.setData({ drawerOpen: false });
    wx.navigateTo({ url: '/pages/login/login' });
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

  // Sign out the CURRENT account. auth.signOut drops its slot and falls back to
  // another stored account when one exists (relaunch chat as that account);
  // otherwise it clears the pointer and we land on login.
  onSignOut: function () {
    if (auth.signOut()) {
      wx.reLaunch({ url: '/pages/chat/chat' });
    } else {
      wx.reLaunch({ url: '/pages/login/login' });
    }
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

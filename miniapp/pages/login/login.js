// Login page. Two flows, both ending in the same auth envelope the backend's
// verify routes return ({ access_token, token_type, expires_in, refresh_token }):
//
//   * WeChat — wx.login() yields a short-lived `code`; POST /wechat/verify calls
//     jscode2session (appid + secret) to resolve the openid, creates-or-gets the
//     user, and returns the tokens. The platform-native primary entry.
//   * Email  — POST /email/login mails a one-time code; POST /email/verify checks
//     it and returns the tokens. Mirrors the web client's second flow
//     (htdoc/src/login.js), so a user without WeChat can still sign in.
var api = require('../../utils/api.js');
var auth = require('../../utils/auth.js');

// Match the server's lenient rule (normalize_email): an '@' with non-empty,
// space-free parts on both sides. No dot required (so "user@demo" is accepted),
// exactly like the web client's emailValid.
var EMAIL_RE = /^[^\s@]+@[^\s@]+$/;
var COOLDOWN_SECONDS = 60;

Page({
  data: {
    loading: false,          // WeChat exchange in flight
    email: '',
    code: '',
    canSend: false,          // email looks valid AND no send/cooldown in progress
    sendLabel: '发送验证码',
    verifying: false,        // /email/verify in flight
    codeError: false,        // tints the code field red after a failed verify
    status: '',              // shared status line (both flows write here)
    drawerOpen: false,       // the settings-only nav drawer
  },

  // Non-reactive handles kept off `data`.
  _cooldownTimer: null,
  _sending: false,

  onUnload: function () {
    if (this._cooldownTimer) { clearInterval(this._cooldownTimer); this._cooldownTimer = null; }
  },

  // -- nav drawer ---------------------------------------------------------
  // Signed out it holds the app-settings group and nothing else; everything else
  // in the chat drawer is session-scoped.
  openDrawer: function () { this.setData({ drawerOpen: true }); },
  closeDrawer: function () { this.setData({ drawerOpen: false }); },

  // A backend change re-points every request; nothing is in flight on this
  // screen, so there is nothing to reload -- just close the drawer so the next
  // sign-in attempt visibly starts against the new server.
  onBackendChange: function () { this.closeDrawer(); },

  // -- shared -------------------------------------------------------------
  // `email` is passed only by the email flow (WeChat login has none); it is
  // stored so the chat menu can show the signed-in account.
  goChat: function (data, email) {
    auth.setAuth(data, email);
    wx.reLaunch({ url: '/pages/chat/chat' });
  },

  // -- WeChat -------------------------------------------------------------
  onWxLogin: function () {
    if (this.data.loading) return;
    var self = this;
    self.setData({ loading: true, status: '' });

    wx.login({
      success: function (res) {
        if (!res.code) { self.fail('微信登录失败：未获取到 code'); return; }
        // noToken: this exchange runs before we hold any app token.
        api.post('/wechat/verify', { code: res.code }, { noToken: true })
          .then(function (data) { self.goChat(data); })
          .catch(function (err) { self.fail((err && err.message) || '登录失败'); });
      },
      fail: function () { self.fail('微信登录失败'); },
    });
  },

  fail: function (msg) {
    this.setData({ loading: false, status: msg });
    wx.showToast({ title: msg, icon: 'none' });
  },

  // -- email + one-time code ---------------------------------------------
  onEmailInput: function (e) {
    this.setData({ email: (e.detail.value || '').trim() });
    this.refreshCanSend();
  },

  // Send code is enabled only for a valid-looking email, except while a request
  // is in flight or the cooldown is running (those states own the button).
  refreshCanSend: function () {
    if (this._sending || this._cooldownTimer) return;
    this.setData({ canSend: EMAIL_RE.test(this.data.email) });
  },

  // Digits only, max six; clear the error tint on edit; auto-submit at six.
  onCodeInput: function (e) {
    var code = (e.detail.value || '').replace(/\D/g, '').slice(0, 6);
    this.setData({ code: code, codeError: false });
    if (code.length === 6) { this.onVerify(); }
  },

  onSendCode: function () {
    var self = this;
    var email = this.data.email.trim();
    if (!EMAIL_RE.test(email)) { this.setData({ status: '请输入有效的电子邮箱' }); return; }

    this._sending = true;
    this.setData({ canSend: false, status: '正在发送验证码…' });
    api.post('/email/login', { email: email }, { noToken: true })
      .then(function () {
        self._sending = false;
        self.setData({ status: '验证码已发送至 ' + email });
        self.startCooldown(COOLDOWN_SECONDS);
      })
      .catch(function (err) {
        self._sending = false;
        self.setData({ status: (err && err.message) || '发送失败' });
        self.refreshCanSend();
      });
  },

  startCooldown: function (seconds) {
    var self = this;
    var remaining = seconds;
    this.setData({ canSend: false, sendLabel: remaining + ' 秒后重发' });
    if (this._cooldownTimer) { clearInterval(this._cooldownTimer); }
    this._cooldownTimer = setInterval(function () {
      remaining -= 1;
      if (remaining <= 0) {
        clearInterval(self._cooldownTimer);
        self._cooldownTimer = null;
        self.setData({ sendLabel: '重新发送' });
        self.refreshCanSend();
      } else {
        self.setData({ sendLabel: remaining + ' 秒后重发' });
      }
    }, 1000);
  },

  onVerify: function () {
    var self = this;
    if (this.data.verifying) return;
    var email = this.data.email.trim();
    var code = this.data.code.trim();
    if (!email) { this.setData({ status: '请输入电子邮箱' }); return; }
    if (!code) { this.setData({ status: '请输入验证码' }); return; }

    this.setData({ verifying: true, status: '正在验证…' });
    api.post('/email/verify', { email: email, code: code }, { noToken: true })
      .then(function (data) {
        if (!data || !data.access_token) {
          self.setData({ verifying: false, status: '登录响应异常' });
          return;
        }
        self.goChat(data, email);
      })
      .catch(function (err) {
        self.setData({
          verifying: false,
          codeError: true,
          status: (err && err.message) || '验证失败',
        });
      });
  },
});

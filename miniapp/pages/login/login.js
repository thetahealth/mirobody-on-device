// WeChat login. wx.login() yields a short-lived `code`; we hand it to the
// backend's POST /wechat/verify, which calls WeChat's jscode2session
// (appid + secret) to resolve the openid, creates-or-gets the user, and
// returns the same auth envelope as /email/verify and /firebase/verify
// ({ access_token, token_type, expires_in, refresh_token }).
var api = require('../../utils/api.js');
var auth = require('../../utils/auth.js');

Page({
  data: {
    loading: false,
  },

  onWxLogin: function () {
    if (this.data.loading) return;
    var self = this;
    self.setData({ loading: true });

    wx.login({
      success: function (res) {
        if (!res.code) {
          self.fail('微信登录失败：未获取到 code');
          return;
        }
        // noToken: this exchange runs before we hold any app token.
        api.post('/wechat/verify', { code: res.code }, { noToken: true })
          .then(function (data) {
            auth.setAuth(data);
            wx.reLaunch({ url: '/pages/chat/chat' });
          })
          .catch(function (err) {
            self.fail((err && err.message) || '登录失败');
          });
      },
      fail: function () {
        self.fail('微信登录失败');
      },
    });
  },

  fail: function (msg) {
    this.setData({ loading: false });
    wx.showToast({ title: msg, icon: 'none' });
  },
});

// Backend connection + app settings for the Mini Program.
//
// baseUrl is prefixed to every request URI (mirrors the web client's net.js
// getBaseUrl()). The Mini Program sends requests to "<baseUrl><uri>", e.g.
// "https://api.example.com/api/chat". A value chosen in the drawer settings
// ("后端地址") is stored and overrides DEFAULT_BASE_URL, mirroring net.js's
// setBaseUrl -- so a build can be pointed at a different backend without a
// rebuild.
//
// IMPORTANT — domain whitelist:
//   WeChat only allows requests to HTTPS domains registered under the Mini
//   Program's "服务器域名" (request legal domains) in the mp.weixin.qq.com
//   console. For local development against the C++ server, open WeChat
//   DevTools -> Details (详情) -> Local settings (本地设置) and tick
//   "不校验合法域名、web-view（业务域名）、TLS 版本以及 HTTPS 证书"
//   so http://localhost:8080 is reachable. Production MUST be HTTPS and
//   whitelisted.
var LANG_KEY     = 'mirobody-x-lang';
var FONT_KEY     = 'mirobody-x-font';       // UI font-size offset (see chat.js fontClass)
var BASE_URL_KEY = 'mirobody-base-url';     // matches the web client's key

// Dev: the local C++ server (config.yml HTTP_PORT 8080).
// Prod: your HTTPS origin, optionally including an HTTP_URI_PREFIX path.
var DEFAULT_BASE_URL = 'http://localhost:8080';

module.exports = {
  DEFAULT_BASE_URL: DEFAULT_BASE_URL,

  // Live getter: the stored override, else the default. Callers read the
  // current value with plain `config.baseUrl` (api.js apiUrl()).
  get baseUrl() {
    return wx.getStorageSync(BASE_URL_KEY) || DEFAULT_BASE_URL;
  },
  setBaseUrl: function (url) {
    if (url) { wx.setStorageSync(BASE_URL_KEY, url); }
    else { wx.removeStorageSync(BASE_URL_KEY); }
  },

  // The conversation language sent with each chat request (the backend agents
  // read this: 'en', 'zh-CN', ...; see the chat body { ..., language }). Chosen
  // from the drawer settings and persisted; defaults to Simplified Chinese.
  get language() {
    return wx.getStorageSync(LANG_KEY) || 'zh-CN';
  },
  setLanguage: function (code) {
    wx.setStorageSync(LANG_KEY, code || 'zh-CN');
  },

  // UI font-size offset (added to the base tier), one of -4/-2/0/2/4 to match
  // the web client's five FontSizeDialog tiers. Applied by the chat page as a
  // font-scale class (WeChat has no rem root font to scale like the web).
  get fontOffset() {
    return Number(wx.getStorageSync(FONT_KEY)) || 0;
  },
  setFontOffset: function (n) {
    wx.setStorageSync(FONT_KEY, Number(n) || 0);
  },
};

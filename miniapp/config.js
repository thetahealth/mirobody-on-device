// Backend connection settings for the Mini Program.
//
// baseUrl is prefixed to every request URI (mirrors the web client's net.js
// getBaseUrl()). The Mini Program sends requests to "<baseUrl><uri>", e.g.
// "https://api.example.com/api/chat".
//
// IMPORTANT — domain whitelist:
//   WeChat only allows requests to HTTPS domains registered under the Mini
//   Program's "服务器域名" (request legal domains) in the mp.weixin.qq.com
//   console. For local development against the C++ server, open WeChat
//   DevTools -> Details (详情) -> Local settings (本地设置) and tick
//   "不校验合法域名、web-view（业务域名）、TLS 版本以及 HTTPS 证书"
//   so http://localhost:8080 is reachable. Production MUST be HTTPS and
//   whitelisted.
var LANG_KEY = 'mirobody-x-lang';

// The conversation language sent with each chat request (the backend agents
// read this: 'en', 'zh-CN', ...; see the chat body { ..., language }). Chosen
// from the menu and persisted; defaults to Simplified Chinese. Exposed as a
// getter so callers read the current value with plain `config.language`.
module.exports = {
  // Dev: the local C++ server (config.yml HTTP_PORT 8080).
  // Prod: your HTTPS origin, optionally including an HTTP_URI_PREFIX path.
  baseUrl: 'http://localhost:8080',

  get language() {
    return wx.getStorageSync(LANG_KEY) || 'zh-CN';
  },
  setLanguage: function (code) {
    wx.setStorageSync(LANG_KEY, code || 'zh-CN');
  },
};

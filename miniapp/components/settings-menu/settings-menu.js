// Shared app-settings menu — the gear button + dropdown that sits on the right
// of the top bar, on BOTH the chat and login screens (identical), mirroring the
// web client's topbar.js buildSettingsMenu. It is intentionally just the app
// settings — language / font size / backend / about — so it reads the same in
// both places; everything session-scoped (history, care circle, account,
// sign out) lives in the chat drawer instead.
//
// Each row writes straight through to config.js (the persisted settings store)
// and then triggers a small event so the host page can react live:
//   bind:langchange    -> { code }   (chat re-labels; language rides each request)
//   bind:fontchange    -> { offset, fontClass }  (chat rescales its bubbles)
//   bind:backendchange -> { baseUrl } (chat reloads providers/history)
var config = require('../../config.js');

// Conversation languages (value drives the chat request `language`; UI copy
// stays Chinese). Kept in step with chat.js LANGS.
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

// Five font tiers (offset -> label + host class), matching the web client's
// FontSizeDialog. The class is applied by the chat page to scale message text.
var FONT_TIERS = [
  { offset: -4, name: '较小', cls: 'fs-smaller' },
  { offset: -2, name: '小',   cls: 'fs-small' },
  { offset: 0,  name: '标准', cls: '' },
  { offset: 2,  name: '大',   cls: 'fs-large' },
  { offset: 4,  name: '较大', cls: 'fs-larger' },
];

function fontIndexOf(offset) {
  for (var i = 0; i < FONT_TIERS.length; i++) {
    if (FONT_TIERS[i].offset === offset) { return i; }
  }
  return 2; // 标准
}
function langIndexOf(code) {
  for (var i = 0; i < LANGS.length; i++) {
    if (LANGS[i].code === code) { return i; }
  }
  return 0;
}

Component({
  data: {
    open: false,
    langNames: LANGS.map(function (l) { return l.name; }),
    langIndex: 0,
    langLabel: '',
    fontNames: FONT_TIERS.map(function (f) { return f.name; }),
    fontIndex: 2,
    fontLabel: '标准',
    backendLabel: '',
    versionLabel: '',
  },

  lifetimes: {
    attached: function () { this.refresh(); },
  },

  methods: {
    // Read the current persisted values into the row hints.
    refresh: function () {
      var li = langIndexOf(config.language);
      var fi = fontIndexOf(config.fontOffset);
      this.setData({
        langIndex: li,
        langLabel: LANGS[li].name,
        fontIndex: fi,
        fontLabel: FONT_TIERS[fi].name,
        backendLabel: config.baseUrl,
        versionLabel: config.APP_VERSION ? ('v' + config.APP_VERSION) : '',
      });
    },

    toggle: function () { this.setData({ open: !this.data.open }); },
    close: function () { this.setData({ open: false }); },

    onLang: function (e) {
      var idx = Number(e.detail.value) || 0;
      var lang = LANGS[idx] || LANGS[0];
      config.setLanguage(lang.code);
      this.setData({ langIndex: idx, langLabel: lang.name, open: false });
      this.triggerEvent('langchange', { code: lang.code });
    },

    onFont: function (e) {
      var idx = Number(e.detail.value) || 0;
      var tier = FONT_TIERS[idx] || FONT_TIERS[2];
      config.setFontOffset(tier.offset);
      this.setData({ fontIndex: idx, fontLabel: tier.name, open: false });
      this.triggerEvent('fontchange', { offset: tier.offset, fontClass: tier.cls });
    },

    // Backend address: WeChat's wx.showModal supports an editable single-line
    // input, so we prompt for the full URL (default = the current one). Empty
    // clears the override back to the built-in default.
    onBackend: function () {
      var self = this;
      this.setData({ open: false });
      wx.showModal({
        title: '后端地址',
        editable: true,
        placeholderText: config.DEFAULT_BASE_URL,
        content: config.baseUrl,
        success: function (r) {
          if (!r.confirm) { return; }
          var url = (r.content || '').trim();
          config.setBaseUrl(url);
          self.setData({ backendLabel: config.baseUrl });
          self.triggerEvent('backendchange', { baseUrl: config.baseUrl });
        },
      });
    },

    onAbout: function () {
      this.setData({ open: false });
      wx.showModal({
        title: '关于',
        content: 'Mirobody' + (config.APP_VERSION ? '\n版本 v' + config.APP_VERSION : ''),
        showCancel: false,
        confirmText: '关闭',
      });
    },
  },
});

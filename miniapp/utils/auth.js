// JWT token storage, the Mini Program counterpart of the web client's
// net.js token helpers. The access token returned by /wechat/verify or
// /email/verify is kept in synchronous storage and replayed on the
// Authorization header by api.js. The signed-in email (when known — the email
// flow has one, WeChat login doesn't) is stored alongside for the menu.
var TOKEN_KEY   = 'mirobody-x-token';
var REFRESH_KEY = 'mirobody-x-refresh';
var EMAIL_KEY   = 'mirobody-x-email';

function getToken() {
  return wx.getStorageSync(TOKEN_KEY) || '';
}

function getEmail() {
  return wx.getStorageSync(EMAIL_KEY) || '';
}

// Persist the auth envelope returned by a verify endpoint
// ({ access_token, refresh_token, expires_in, token_type }). `email` is
// optional — pass it for the email flow so the menu can show the account.
function setAuth(data, email) {
  if (data && data.access_token) {
    wx.setStorageSync(TOKEN_KEY, data.access_token);
  }
  if (data && data.refresh_token) {
    wx.setStorageSync(REFRESH_KEY, data.refresh_token);
  }
  if (email) {
    wx.setStorageSync(EMAIL_KEY, email);
  }
}

function clearAuth() {
  wx.removeStorageSync(TOKEN_KEY);
  wx.removeStorageSync(REFRESH_KEY);
  wx.removeStorageSync(EMAIL_KEY);
}

module.exports = {
  getToken: getToken,
  getEmail: getEmail,
  setAuth: setAuth,
  clearAuth: clearAuth,
};

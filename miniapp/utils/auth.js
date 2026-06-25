// JWT token storage, the Mini Program counterpart of the web client's
// net.js token helpers. The access token returned by /wechat/verify is kept
// in synchronous storage and replayed on the Authorization header by api.js.
var TOKEN_KEY   = 'mirobody-x-token';
var REFRESH_KEY = 'mirobody-x-refresh';

function getToken() {
  return wx.getStorageSync(TOKEN_KEY) || '';
}

// Persist the auth envelope returned by a verify endpoint
// ({ access_token, refresh_token, expires_in, token_type }).
function setAuth(data) {
  if (data && data.access_token) {
    wx.setStorageSync(TOKEN_KEY, data.access_token);
  }
  if (data && data.refresh_token) {
    wx.setStorageSync(REFRESH_KEY, data.refresh_token);
  }
}

function clearAuth() {
  wx.removeStorageSync(TOKEN_KEY);
  wx.removeStorageSync(REFRESH_KEY);
}

module.exports = {
  getToken: getToken,
  setAuth: setAuth,
  clearAuth: clearAuth,
};

// WeChat WeRun (微信运动) step sync — the client half of POST /wechat/werun.
//
// wx.getWeRunData() returns the user's last ~31 days of daily steps as an encrypted
// blob (encryptedData + iv); wx.login() yields a short-lived code. We hand both to
// the backend (health::WeRunService), which exchanges the code for the session_key,
// decrypts the blob, and stores the steps as FHIR Observations. The user must be
// logged in (api.post attaches the bearer token) and grant the WeRun scope —
// wx.getWeRunData prompts for it on first use.
//
// Usage (e.g. from a "Sync steps" button or chat onLoad):
//   require('../../utils/werun.js').syncSteps()
//     .then(function (r) { /* r = { posted, failed } */ })
//     .catch(function (e) { /* e.message */ });
var api = require('./api.js');

// Promisified wx.login → resolves with the code.
function login() {
  return new Promise(function (resolve, reject) {
    wx.login({
      success: function (res) {
        if (res && res.code) resolve(res.code);
        else reject(new Error('wx.login: no code'));
      },
      fail: function (e) { reject(new Error((e && e.errMsg) || 'wx.login failed')); },
    });
  });
}

// Promisified wx.getWeRunData → resolves with { encryptedData, iv }.
function getWeRunData() {
  return new Promise(function (resolve, reject) {
    wx.getWeRunData({
      success: function (res) {
        resolve({ encryptedData: res.encryptedData, iv: res.iv });
      },
      fail: function (e) {
        reject(new Error((e && e.errMsg) || 'getWeRunData failed'));
      },
    });
  });
}

// Fetch the last ~31 days of WeChat steps and push them to the backend for FHIR
// ingestion. Resolves with the server's { posted, failed } summary. A fresh code is
// paired with each blob so the backend never has to store the session_key.
function syncSteps() {
  return Promise.all([login(), getWeRunData()]).then(function (r) {
    var code = r[0];
    var blob = r[1];
    return api.post('/wechat/werun', {
      code: code,
      encryptedData: blob.encryptedData,
      iv: blob.iv,
    });
  });
}

module.exports = {
  syncSteps: syncSteps,
};

# Mirobody WeChat Mini Program

A native WeChat Mini Program (微信小程序) frontend for the Mirobody backend. No
build step — it opens directly in WeChat DevTools. It mirrors the web client in
[`htdoc/`](../htdoc): the same `{code, msg, data}` envelope, the same `Bearer`
JWT auth, and the same streamed `/api/chat` agent protocol.

## Layout

```
miniapp/
  app.js / app.json / app.wxss   global app + page registry + base styles
  project.config.json            DevTools project (set your AppID here)
  config.js                      backend baseUrl + default language
  utils/
    auth.js                      JWT storage (wx.getStorageSync)
    api.js                       wx.request wrapper: post() + SSE stream()
  pages/
    login/                       wx.login() -> POST /wechat/verify -> token
    chat/                        provider picker + streamed agent chat
```

## Run it

1. Install **WeChat DevTools** (微信开发者工具) and open this `miniapp/` folder as
   a project (Mini Program / 小程序).
2. Put your Mini Program **AppID** in `project.config.json` (`appid`). Use a test
   AppID if you don't have one yet.
3. Set the backend in [`config.js`](config.js):
   - Dev: `baseUrl: 'http://localhost:8080'` (the C++ server's `HTTP_PORT`).
   - Prod: your HTTPS origin (+ any `HTTP_URI_PREFIX` path).
4. For a local backend, in DevTools tick
   **Details (详情) → Local settings (本地设置) →
   "不校验合法域名、web-view、TLS 版本以及 HTTPS 证书"**.
   WeChat otherwise only allows HTTPS domains whitelisted under
   服务器域名 in the mp.weixin.qq.com console — required for production.

## Backend dependency: `POST /wechat/verify` (not yet implemented)

The login page calls an endpoint that does **not exist in the C++ server yet**.
It is the WeChat sibling of [`/firebase/verify` and `/email/verify`](../src/user/service.cpp)
and should be added to `UserService::register_routes`. Contract the frontend
expects:

**Request** (no auth):
```json
POST /wechat/verify
{ "code": "<the code from wx.login()>" }
```

**Server behavior:**
1. Exchange `code` via WeChat's `jscode2session`:
   `https://api.weixin.qq.com/sns/jscode2session?appid=<APPID>&secret=<SECRET>&js_code=<code>&grant_type=authorization_code`
   → `{ openid, unionid?, session_key }`.
2. Create-or-get the user **by `wechat_openid`** (the column already exists on
   `health_app_user`; add an `add_or_get_user_by_openid` alongside the
   email-based one). No email is involved.
3. Return the standard auth envelope (identical to the other verify routes):

**Response:**
```json
{ "code": 0, "msg": "ok",
  "data": { "access_token": "...", "token_type": "Bearer",
            "expires_in": 3600, "refresh_token": "..." } }
```

Two new config keys are needed for the AppID/secret (e.g. `WECHAT_APPID` /
`WECHAT_SECRET`), read in `src/config/config.*` like the existing provider keys.

## What the chat page uses (already implemented in the backend)

- `POST /api/providers` — `[{ code: "agent/provider", name }]`, no auth. Fills
  the picker; `code` is split on `/` into the `agent` + `provider` fields.
- `POST /api/chat` — auth required. Body
  `{ agent, provider, question, language }`; response is SSE with events
  `{ type: "reply" | "thinking" | "costStatistics" | "error", content }`.
  Streamed via `wx.request({ enableChunked: true })` + `onChunkReceived`
  (`utils/api.js` `stream()`), the Mini Program analog of the web client's
  `fetch` + `ReadableStream`.

A 401 on any authenticated call clears the stored token and returns to login.

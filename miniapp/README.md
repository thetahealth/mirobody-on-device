# Mirobody WeChat Mini Program

A native WeChat Mini Program (微信小程序) frontend for the Mirobody backend. No
build step — it opens directly in WeChat DevTools. It mirrors the web client in
[`htdoc/`](../htdoc): the same `{code, msg, data}` envelope, the same `Bearer`
JWT auth, and the same streamed `/api/chat` agent protocol.

## Layout

The UI follows the web client's "Theta Health" design (warm cream surface, navy
accents, serif brand title). Login offers **WeChat** and an **email one-time
code** flow; the chat page's hamburger opens a **left nav drawer** (the app's
single nav home, mirroring the web client): new chat, past sessions
(resume/delete), WeChat-steps sync, care circle, language, sign out.

```
miniapp/
  app.js / app.json / app.wxss   global app + page registry + base styles
  project.config.json            DevTools project (set your AppID here)
  config.js                      backend baseUrl + conversation language (persisted)
  utils/
    auth.js                      JWT + email storage (wx.getStorageSync)
    api.js                       wx.request wrapper: post() + get() + SSE stream()
    werun.js                     WeChat steps sync -> POST /wechat/werun
  pages/
    login/                       WeChat (POST /wechat/verify) + email code
                                 (POST /email/login, /email/verify)
    chat/                        provider picker + streamed agent chat + nav drawer
                                 (GET /api/history, /api/conversation;
                                  POST /api/history/delete)
    circle/                      care circle: list/create/invite/accept/decline/
                                 remove/delete/rename/nickname/role/health-sharing
                                 (/api/circle/*)
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

## Backend dependency: `POST /wechat/verify` (implemented)

The login page calls `POST /wechat/verify`, the WeChat sibling of
[`/firebase/verify` and `/email/verify`](../src/user/service.cpp). It is
**implemented** in the C++ server (`UserService::on_wechat_verify`, registered
in `register_routes`) and handles the Mini Program flow the login page uses.
Contract:

**Request** (no auth):
```json
POST /wechat/verify
{ "code": "<the code from wx.login()>" }
```

The server also accepts an optional `"flow": "web" | "app"` to distinguish the
WeChat web (qrconnect / oauth2) and mobile-app (OpenSDK) code sources from the
Mini Program flow (the default, used here). Each flow uses its own credentials,
falling back to the Mini Program pair when unset.

**Server behavior (Mini Program flow):**
1. Exchange `code` via WeChat's `jscode2session`:
   `https://api.weixin.qq.com/sns/jscode2session?appid=<APPID>&secret=<SECRET>&js_code=<code>&grant_type=authorization_code`
   → `{ openid, unionid?, session_key }`.
2. Create-or-get the user by the WeChat identity (`add_or_get_wechat_user`,
   keyed on openid — preferring the unionid when present). No email is involved.
3. Return the standard auth envelope (identical to the other verify routes):

**Response:**
```json
{ "code": 0, "msg": "ok",
  "data": { "access_token": "...", "token_type": "Bearer",
            "expires_in": 3600, "refresh_token": "..." } }
```

The config keys for the AppID/secret (`WECHAT_APPID` / `WECHAT_SECRET`) are read
in `src/config/config.*` like the existing provider keys; set them to your Mini
Program's credentials before the flow will work.

## What the chat page uses (already implemented in the backend)

- `POST /api/providers` — auth required (it exposes the configured model names).
  Returns groups `[{ agent, providers[] }]`; the picker flattens them to one
  `{ agent, provider }` per model (the default agent's group has an empty `agent`).
- `POST /api/chat` — auth required. Body
  `{ agent, provider, question, language }`; response is SSE with events
  `{ type: "reply" | "thinking" | "costStatistics" | "error", content }`.
  Streamed via `wx.request({ enableChunked: true })` + `onChunkReceived`
  (`utils/api.js` `stream()`), the Mini Program analog of the web client's
  `fetch` + `ReadableStream`.

A 401 on any authenticated call clears the stored token and returns to login.

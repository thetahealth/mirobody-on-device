// Electron main process: embed the mirobody C++ server in-process via koffi,
// then load the bundled web client (res/htdoc) from the server's loopback HTTP
// port into a BrowserWindow. The renderer is an ordinary web app talking to the
// embedded server over 127.0.0.1 — exactly like the standalone desktop build,
// only the server lives inside this process instead of a separate binary.
const path = require('path');
const http = require('http');
const { app, BrowserWindow, shell } = require('electron');
const { MiroServer } = require('./mirobody');

// Fixed loopback port (the C API reports the configured port, not an ephemeral
// one — see mirobody.js). Override with MIROBODY_PORT if it clashes.
const PORT = Number(process.env.MIROBODY_PORT || 8421);

let server = null;
let win = null;

// Packaged: extraResources land under process.resourcesPath. Dev: read straight
// from the repo (one level up from electron/).
function resDir() {
  return app.isPackaged
    ? path.join(process.resourcesPath, 'res')
    : path.join(__dirname, '..', 'res');
}
function libDir() {
  return app.isPackaged
    ? path.join(process.resourcesPath, 'native')
    : path.join(__dirname, '..', 'build-shared');
}
function configPath() {
  // The embedded server runs off the repo-root config.example.yml (the committed
  // defaults template) — there's no separate desktop config. Real per-machine
  // overrides (paths, port, loopback host) are injected as env vars in
  // startServer(), which the config store honors at highest precedence.
  return app.isPackaged
    ? path.join(process.resourcesPath, 'config.example.yml')
    : path.join(__dirname, '..', 'config.example.yml');
}

function startServer() {
  const userData = app.getPath('userData');

  // The config store reads env vars at HIGHEST precedence (over config.yml), so
  // use them for the paths that must be absolute at runtime: the read-only
  // resources dir (HTTP_ROOT / SQL_DIR, otherwise resolved against an
  // unpredictable cwd) and a writable location for uploads. The SQLite DB path
  // comes from the dataDir argument below (-> <userData>/mirobody.db). The listen
  // port is no longer a start() argument — set it the same env-var way.
  process.env.HTTP_HOST = '127.0.0.1';
  process.env.HTTP_PORT = String(PORT);
  process.env.HTTP_ROOT = path.join(resDir(), 'htdoc');
  process.env.SQL_DIR = path.join(resDir(), 'sql');
  process.env.LOCAL_STORAGE_DIR = path.join(userData, 'uploads');

  server = new MiroServer(libDir());
  if (!server.start(configPath(), userData)) {
    throw new Error('mirobody_start failed (see native log on stderr)');
  }
  return server.port();
}

// The server binds asynchronously; poll GET / until it answers before pointing
// the window at it, so the first load isn't a connection-refused error page.
function waitForHttp(port, tries = 50) {
  return new Promise((resolve) => {
    let attempt = 0;
    const probe = () => {
      const req = http.get({ host: '127.0.0.1', port, path: '/' }, (res) => {
        res.resume();
        resolve(true);
      });
      req.on('error', retry);
      req.setTimeout(200, () => { req.destroy(); retry(); });
    };
    const retry = () => {
      if (++attempt >= tries) return resolve(false);
      setTimeout(probe, 100);
    };
    probe();
  });
}

// Hosts whose popups are OAuth sign-in windows: these must open as a child
// BrowserWindow (not the system browser) so the popup keeps its `window.opener`
// relationship and can postMessage the result back to the renderer, which is how
// the Google (Firebase) and Apple (usePopup) JS SDKs complete sign-in. WeChat
// uses a full-page redirect (not window.open), so it isn't affected either way.
const AUTH_POPUP_HOSTS = [
  'appleid.apple.com',
  'appleid.cdn-apple.com',
  'accounts.google.com',
  'apis.google.com',
  'firebaseapp.com',     // <project>.firebaseapp.com auth handler
  'open.weixin.qq.com',
];

function isAuthPopupUrl(url) {
  try {
    const host = new URL(url).hostname;
    return AUTH_POPUP_HOSTS.some((h) => host === h || host.endsWith('.' + h));
  } catch (e) {
    return false;
  }
}

function createWindow(port) {
  win = new BrowserWindow({
    width: 1100,
    height: 800,
    webPreferences: {
      contextIsolation: true,
      nodeIntegration: false,
      preload: path.join(__dirname, 'preload.js'),
    },
  });
  win.loadURL(`http://127.0.0.1:${port}/`);
  // Sign-in popups (Apple / Google) must open in-app as a child window so they
  // can postMessage their result back to the opener; any other window.open
  // (genuine external links) goes to the system browser instead.
  win.webContents.setWindowOpenHandler(({ url }) => {
    if (isAuthPopupUrl(url)) {
      return {
        action: 'allow',
        overrideBrowserWindowOptions: {
          width: 480,
          height: 640,
          webPreferences: { contextIsolation: true, nodeIntegration: false },
        },
      };
    }
    shell.openExternal(url);
    return { action: 'deny' };
  });
}

app.whenReady().then(async () => {
  let port;
  try {
    port = startServer();
  } catch (e) {
    console.error(e);
    app.quit();
    return;
  }

  await waitForHttp(port);
  createWindow(port);

  app.on('activate', () => {
    if (BrowserWindow.getAllWindows().length === 0) createWindow(port);
  });
});

app.on('window-all-closed', () => {
  if (process.platform !== 'darwin') app.quit();
});

// Stop the embedded server cleanly on exit (joins its threads).
app.on('will-quit', () => {
  if (server) {
    server.stop();
    server = null;
  }
});

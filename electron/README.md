# Mirobody Electron desktop

An Electron wrapper that **embeds the mirobody C++ server in-process** and shows
the existing web client ([`htdoc/`](../htdoc)) as its UI. No second process, no
HTTP bridge to manage: the main process loads the C-ABI shared library
([`libmirobody`](../src/mirobody.h)) with [koffi](https://koffi.dev) FFI, starts
the server on `127.0.0.1`, and a `BrowserWindow` loads `http://127.0.0.1:<port>/`.

koffi is a *prebuilt* FFI, so the binding survives Electron's Node-version bumps
with no `electron-rebuild` step — unlike a native N-API addon.

```
electron/
  main.js        # start embedded server, wait for it, open the window; on-device LLM IPC
  mirobody.js    # koffi wrapper over the 4 functions in src/mirobody.h
  ondevice.js    # main-process on-device LLM: Gemma 4 (GGUF) via node-llama-cpp + model download
  preload.js     # context-isolated; desktop marker + window.ondevice bridge
  package.json   # electron + electron-builder + koffi + node-llama-cpp; packaging config
```

## Prerequisites (build the two artifacts it loads)

1. **The shared library** — built by the repo-root scripts into `build-shared/`
   (`mirobody.dll` / `libmirobody.so` / `libmirobody.dylib`). Defaults to the
   self-contained **SQLite** backend, so the desktop app needs no external
   database:

   ```cmd
   ..\build-shared.cmd          :: Windows  -> ..\build-shared\mirobody.dll
   ```
   ```sh
   ../build-shared.sh           # Linux/macOS -> ../build-shared/libmirobody.{so,dylib}
   ```

2. **The web client** — built into [`res/htdoc`](../res/htdoc) (already present;
   rebuild after web changes):

   ```sh
   cd ../htdoc && npm install && npm run build      # -> ../res/htdoc
   ```

## Run (dev)

One command (builds the web bundle, installs deps, then runs or packages):

```cmd
build-electron.cmd            :: Windows  (dist to package; clean to reinstall deps)
```
```sh
./build-electron.sh           # Linux/macOS
```

It does **not** build the shared library (step 1 above) — do that once first. Or run
the steps by hand:

```sh
npm install
npm start
```

`main.js` reads the library from `../build-shared`, resources from `../res`, and
the repo-root [`config.example.yml`](../config.example.yml) directly — no
packaging needed. Enable demo login there first (uncomment `EMAIL_PREDEFINE_CODES`,
off by default), then sign in with `demo1@mirobody.ai` / `777777`; add an LLM key
there to enable cloud chat, or pick
the **on-device** provider (no key needed — see below). Per-machine
overrides (paths, port, loopback host) come from env vars set in `main.js`, which
the config store honors above the file.

## On-device LLM (private chat, desktop-only)

The renderer is the **shared** web client (`htdoc/`), sandboxed with no Node access,
so the on-device model runs in the **main process** and is bridged to the page via
`preload.js` (`window.ondevice`). The chat picker then offers **"Gemma 4 · On-device"**
— private, offline, no LLM key needed. Because it's gated on that bridge, it appears
only in the desktop app, never when `htdoc/` is served to a plain browser.

- **Engine** (`ondevice.js`): Gemma 4 (GGUF) via **node-llama-cpp**, loaded lazily
  (ESM `import()`); it streams tokens back as the same `reply` chunks the SSE path
  emits, so the chat UI is unchanged. Missing dependency ⇒ graceful "unavailable".
- **Model**: the GGUF is **not bundled** — downloaded on demand from Hugging Face
  (`createModelDownloader`, with progress) into Electron's `userData`. Confirm the
  `MODEL_URI` in `ondevice.js` points at a real Gemma 4 GGUF before shipping.
- **Dependency**: `node-llama-cpp` (in `package.json`). `npm install` fetches a
  prebuilt native binary and electron-builder bundles it. Unlike koffi it's a native
  N-API addon, so a major Node/Electron bump may need a rebuild.

## Package (electron-builder)

```sh
npm run dist        # -> dist/
```

`package.json`'s `build.extraResources` bundles, into the app's `resources/`:

| Source                | Packaged at          | Purpose                                  |
| --------------------- | -------------------- | ---------------------------------------- |
| `../build-shared/*`   | `native/`            | the shared library (current OS/arch)     |
| `../res/sql`          | `res/sql`            | SQLite DDL applied at startup            |
| `../res/htdoc`        | `res/htdoc`          | the web UI served to the window          |
| `../config.example.yml` | `config.example.yml` | server config (committed defaults template) |

At runtime `main.js` switches its paths to `process.resourcesPath` when
`app.isPackaged`, so the same code works in dev and packaged.

## How it wires up (notes & gotchas)

- **Writable vs read-only paths.** The resources dir is read-only in a packaged
  app, so `main.js` sets the SQLite file (via the `dataDir` argument →
  `<userData>/mirobody.db`) and `LOCAL_STORAGE_DIR` under Electron's `userData`.
  `HTTP_ROOT`/`SQL_DIR` point at the read-only bundled `res/`. These are passed
  as **env vars**, which the config store honors at highest precedence.
- **Fixed port.** The C API reports the *configured* port, not an OS-assigned
  one, so a fixed port (`8421`, override with `MIROBODY_PORT`) is used and read
  back — port `0`/ephemeral would leave the URL unknown.
- **FFI lives in the main process** only (`contextIsolation: true`,
  `nodeIntegration: false` in the renderer).
- **Cross-platform.** Build the shared library on each OS/arch you ship; the
  `extraResources` filter copies whichever `mirobody.dll`/`libmirobody.*` is
  present, and `mirobody.js` picks the right name per `process.platform`.
- **Security note.** The bundled `config.example.yml` ships placeholder `JWT_KEY`
  / secrets — replace these before distributing. Demo login
  (`EMAIL_PREDEFINE_CODES`) is commented out by default; if you enable it for
  local use, do not ship it enabled (anyone could sign in with the well-known
  codes). It also carries the full server template (e.g. a `PG_*` block); those
  keys are inert under the desktop's SQLite-backed shared library.
```

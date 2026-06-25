// Thin koffi wrapper around the C-ABI functions in src/mirobody.h. This is
// the same surface bindings/node/index.js drives — koffi is a prebuilt FFI, so
// it loads the shared library without compiling against Electron's Node ABI
// (no electron-rebuild needed when Electron's Node version changes).
const path = require('path');
const koffi = require('koffi');

// Platform-specific shared-library file name produced by build-shared.{cmd,sh}.
function libFileName() {
  if (process.platform === 'win32') return 'mirobody.dll';
  if (process.platform === 'darwin') return 'libmirobody.dylib';
  return 'libmirobody.so';
}

class MiroServer {
  // `libDir` is the directory containing the shared library.
  constructor(libDir) {
    const libPath = path.join(libDir, libFileName());
    this.lib = koffi.load(libPath);

    // The opaque mirobody_server_t* is marshalled as a plain void*; koffi hands
    // back null for a NULL pointer, so `!!handle` is the success test.
    this._start = this.lib.func(
      'void* mirobody_start(const char*, const char*)');
    this._stop = this.lib.func('void mirobody_stop(void*)');
    this._isRunning = this.lib.func('int mirobody_is_running(void*)');
    this._port = this.lib.func('int mirobody_listen_port(void*)');

    this.handle = null;
  }

  // Start the embedded server. `configPath`/`dataDir` may be empty/null (NULL ->
  // config-file or compiled defaults). `dataDir` is where the on-device SQLite
  // file (mirobody.db) is written. The listen port comes from config — set the
  // HTTP_PORT env var (or config.yml) before constructing this; the C API reports
  // that *configured* port back via port(), not an OS-assigned ephemeral one.
  // Returns true on success.
  start(configPath, dataDir) {
    this.handle = this._start(configPath || null, dataDir || null);
    return !!this.handle;
  }

  isRunning() {
    return this.handle ? this._isRunning(this.handle) !== 0 : false;
  }

  port() {
    return this.handle ? this._port(this.handle) : -1;
  }

  stop() {
    if (this.handle) {
      this._stop(this.handle);
      this.handle = null;
    }
  }
}

module.exports = { MiroServer };

// Node.js binding + runnable example for libmirobody, using koffi (a prebuilt
// FFI module — no native compiler needed). koffi loads the same C-ABI shared
// library every other host uses and calls the functions from mirobody.h.
//
// Usage: node index.js [library-path] [dataDir]
//
// The listen port comes from config (HTTP_PORT env / config.yml, default 8080);
// the demo reads the bound port back via mirobody_listen_port. Run from the repo
// root so the default config's sql_dir ("res/sql") resolves, with the native
// dependency dir on PATH (Windows) / LD_LIBRARY_PATH so the DLL/.so's transitive
// deps load. See bindings/README.md.

const koffi = require('koffi');
const path = require('path');
const os = require('os');

const libPath = process.argv[2] ||
  path.join(__dirname, '..', '..', 'build-shared',
    process.platform === 'win32' ? 'mirobody.dll' : 'libmirobody.so');
const dataDir = process.argv[3] || os.tmpdir();

const lib = koffi.load(libPath);

// Opaque handle is just a void* passed back into the other calls.
const mirobody_start = lib.func(
  'void* mirobody_start(const char*, const char*)');
const mirobody_stop = lib.func('void mirobody_stop(void*)');
const mirobody_is_running = lib.func('int mirobody_is_running(void*)');
const mirobody_listen_port = lib.func('int mirobody_listen_port(void*)');

(async () => {
  // koffi marshals JS strings to const char*; null -> NULL (use the default).
  const handle = mirobody_start(null, dataDir);
  if (!handle) {
    console.error('mirobody_start returned NULL (see native log)');
    process.exit(1);
  }

  console.log(`started: running=${mirobody_is_running(handle)} port=${mirobody_listen_port(handle)}`);

  await new Promise((r) => setTimeout(r, 400));
  try {
    const res = await fetch(`http://127.0.0.1:${mirobody_listen_port(handle)}/`);
    console.log(`GET / -> HTTP ${res.status}`);
  } catch (e) {
    console.log('GET / error:', e.message);
  }

  mirobody_stop(handle);
  console.log(`stopped: running=${mirobody_is_running(handle)}`);
})();

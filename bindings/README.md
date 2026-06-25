# mirobody — language bindings (Java, Go, C#, Node, Rust)

Every non-C language consumes mirobody the same way: through **one C-ABI shared
library**, `libmirobody`, which exports the functions in
[`src/mirobody.h`](../src/mirobody.h):

```c
// Server lifecycle. LLM keys and the listen port come from the config the
// server loads (OPENAI_API_KEY / GOOGLE_API_KEY / HTTP_PORT, which also read
// their environment variables); read the bound port back with _listen_port.
mirobody_server_t* mirobody_start(config_path, data_dir);
void mirobody_stop(mirobody_server_t*);
int  mirobody_is_running(mirobody_server_t*);
int  mirobody_listen_port(mirobody_server_t*);

// Discover the "Agent/model" pairs available to chat with (newline-separated).
const char* mirobody_get_providers(void);

// Serverless one-shot LLM + MCP chat turn, streamed through a callback. The
// `provider` is an "Agent/model" pair from mirobody_get_providers (or "").
int  mirobody_chat(provider, message, user_id, on_event, user_data);
```

That library is implemented in [`src/platform/c_api.cpp`](../src/platform/c_api.cpp)
and built by the `mirobody_shared` CMake target. There is nothing language-specific
in it — Java, Go, C#, Rust, Node, Ruby, Python (via ctypes) all just `dlopen`/load
it and call those symbols through their FFI.

## Building the library

```
build-shared.cmd        :: Windows  -> build-shared\mirobody.dll (+ mirobody.lib)
./build-shared.sh       #  Linux/macOS -> build-shared/libmirobody.so | .dylib
```

The build uses the SQLite backend and, on Windows, the **fully-static
`x64-windows-static` triplet** (`/MT`). The result depends only on system DLLs —
no vcpkg DLLs and no `vcruntime`/`ucrtbase` — so it is **self-contained**: nothing
extra goes on `PATH`, and it loads cleanly into a JVM (see the Java note below).
On Linux/macOS the default triplets already link the deps statically. The Windows
export table is pinned to the six C functions via
[`src/platform/mirobody.def`](../src/platform/mirobody.def).

When `JAVA_HOME` is set, `build-shared.cmd`/`.sh` also builds **`mirobody_jni`**
(`mirobody_jni.dll` / `libmirobody_jni.so`), the JNI shim
([`src/platform/jni_bridge.cpp`](../src/platform/jni_bridge.cpp)).

> The server reads SQL DDL from `sql_dir` (`res/sql` by default) and `config.yml`,
> resolved relative to the **working directory**. Run the examples from the repo
> root, or pass an absolute `config_path`.

All five bindings below were verified end-to-end against the static build with
nothing but the repo on hand — `started → GET / 200 → stopped`. The server's
listen port comes from config (default 8080); each demo reads the bound port back
via `mirobody_listen_port`. To run on a different port, set `HTTP_PORT` in the
environment (or `config.yml`) before launching — e.g. `$env:HTTP_PORT=18101`.

## C — chat (serverless LLM + MCP) — verified ✅

[`c/chat_demo.c`](c/chat_demo.c) is the example for the **`mirobody_chat`** and
**`mirobody_get_providers`** exports. It starts no server: both load the
process-wide config automatically on first use. The demo first calls
`mirobody_get_providers()`, which returns the available `"Agent/model"` pairs
newline-separated, then hands one of those strings straight to `mirobody_chat`,
which streams one assistant turn — `reply` text, `thinking`, MCP `query*` tool
steps, a terminal `costStatistics`, or an `error` — through a C callback. The
opaque `user_data` pointer you pass is threaded back into every callback
invocation, so it carries your context with no globals (here, a struct that
assembles the answer). Provider failures (e.g. a missing API key) arrive as an
`error` event rather than a non-zero return.

```powershell
# from the repo root, in a VS Developer prompt (links the import lib):
cl /nologo /I src bindings\c\chat_demo.c /Fo:build\ /Fe:build\chat_demo.exe /link build\mirobody.lib
$env:PATH = "$PWD\build;$env:PATH"     # so the DLL's transitive deps load
.\build\chat_demo.exe "What is a healthy resting heart rate?"
# pick a provider explicitly:  chat_demo.exe "hi" Base/gemini-2.5-flash
```

```sh
# Linux/macOS, from the repo root, against the shared build:
cc -I src bindings/c/chat_demo.c -Lbuild-shared -lmirobody -o build-shared/chat_demo
./build-shared/chat_demo "What is a healthy resting heart rate?"
```

The same call works from any FFI host — a function pointer for `on_event` and a
context pointer for `user_data` (Go `syscall.NewCallback`, C# delegate, Rust
`extern "C" fn`, Node koffi `koffi.register`, …).

## Go — verified ✅

[`go/main.go`](go/main.go) binds with **zero cgo** on Windows: `syscall.NewLazyDLL`
loads the DLL through the OS loader by absolute path and calls the exports.

```powershell
go build -o build-shared\go_demo.exe .\bindings\go
.\build-shared\go_demo.exe "$PWD\build-shared\mirobody.dll"   # from the repo root
```

**POSIX (Linux/macOS):** use cgo instead of `syscall`:

```go
/*
#cgo CFLAGS:  -I${SRCDIR}/../../src
#cgo LDFLAGS: -L${SRCDIR}/../../build-shared -lmirobody
#include "mirobody.h"
*/
import "C"
// h := C.mirobody_start(nil, cDataDir)
```

## C# / .NET — verified ✅

[`csharp/Mirobody.cs`](csharp/Mirobody.cs) binds via P/Invoke (`[DllImport("mirobody")]`),
with `LPUTF8Str` marshalling for the `const char*` arguments. [`csharp/Program.cs`](csharp/Program.cs)
is the example, [`csharp/mirobody-demo.csproj`](csharp/mirobody-demo.csproj) the project.

```powershell
$env:PATH = "$PWD\build-shared;$env:PATH"     # so DllImport finds mirobody.dll
dotnet run --project bindings/csharp   # run from the repo root
```

CoreCLR doesn't shadow the system C runtime, so this runs in-process directly.
(If you have only the .NET *runtime* and no SDK, build with Roslyn `csc` against
the shared framework and run with `dotnet mirobody-demo.dll` — see the project file.)

## Node.js — verified ✅

[`node/index.js`](node/index.js) binds via [koffi](https://koffi.dev/), a prebuilt
FFI module (no native compiler needed).

```powershell
cd bindings/node; npm install; cd ../..               # pulls koffi (prebuilt)
node bindings/node/index.js "$PWD\build-shared\mirobody.dll"   # from the repo root
```

## Rust — verified ✅

[`rust/src/main.rs`](rust/src/main.rs) is a plain `extern "C"` block;
[`rust/build.rs`](rust/build.rs) links `mirobody.lib` from `build-shared/`
(override with `MIROBODY_LIB_DIR`). Dependency-free — the HTTP probe uses
`std::net`.

```powershell
cargo build --release --manifest-path bindings/rust/Cargo.toml
$env:PATH = "$PWD\build-shared;$env:PATH"             # so mirobody.dll loads at runtime
.\bindings\rust\target\release\mirobody-demo.exe   # from the repo root
```

## Java — verified ✅ (JNI and FFM)

Two bindings, both working in-process on desktop HotSpot against the static build:

- **JNI** (recommended): [`java/ai/thetahealth/mirobody/NativeBridge.java`](java/ai/thetahealth/mirobody/NativeBridge.java)
  + [`java/JniDemo.java`](java/JniDemo.java), backed by `mirobody_jni.dll`
  ([`src/platform/jni_bridge.cpp`](../src/platform/jni_bridge.cpp)) — the same
  `NativeBridge` class the Android app binds. No `--enable-preview`.
- **Panama FFM**: [`java/Mirobody.java`](java/Mirobody.java) + [`java/Demo.java`](java/Demo.java),
  pure JDK, no jars (a preview feature on JDK 21, so `--enable-preview`).

```powershell
# JNI (mirobody_jni.dll is built by build-shared.cmd when JAVA_HOME is set):
javac -d build-shared\jniout bindings\java\ai\thetahealth\mirobody\NativeBridge.java bindings\java\JniDemo.java
java -Djava.library.path="$PWD\build-shared" -cp build-shared\jniout JniDemo 18121

# FFM (loads mirobody.dll by absolute path):
javac --release 21 --enable-preview -d build-shared\ffmout bindings\java\Mirobody.java bindings\java\Demo.java
java --enable-preview --enable-native-access=ALL-UNNAMED -cp build-shared\ffmout Demo "$PWD\build-shared\mirobody.dll"
```

> **Why the static build matters for Java.** A *dynamically*-linked
> `mirobody.dll` segfaults inside `lws_create_context` under HotSpot — both JNI
> and FFM, no `hs_err`. Root cause: `java.exe` ships its own older C-runtime DLLs
> (`vcruntime140.dll`, `msvcp140.dll`, `ucrtbase.dll`) in its `bin\`, and Windows
> resolves a dependency's implicit CRT imports from the launching EXE's directory
> first — so libwebsockets/OpenSSL bind to the JDK's mismatched CRT and fast-fail.
> The static build links the CRT into `mirobody*.dll` (no `ucrtbase` import at
> all), which sidesteps this entirely. Go/Python/.NET/Node don't ship a shadowing
> CRT, so they were never affected.

The demos force HTTP/1.1 (`HttpClient.Version.HTTP_1_1`) because the server speaks
HTTP/1.1 and `java.net.http` otherwise negotiates HTTP/2.

## … and so forth

The same `libmirobody` serves every other FFI host — each is ~20 lines:

| Language | Mechanism |
| -------- | --------- |
| Ruby   | `Fiddle` (stdlib) or the `ffi` gem |
| Python | `ctypes` — or the dedicated pybind11 wheel, see [`../python/`](../python) |

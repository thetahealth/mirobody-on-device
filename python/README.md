# mirobody — Python wheel

Python bindings for the on-device server, built as a CPython extension
(`mirobody._mirobody`, a pybind11 module wrapping the C++ `mirobody::Server`)
and packaged into a `.whl` by [scikit-build-core](https://scikit-build-core.readthedocs.io/).

The build defaults to the **SQLite** database backend so the wheel is
self-contained, like the iOS/Android builds.

## Layout

| Path                              | Role                                                        |
| --------------------------------- | ----------------------------------------------------------- |
| `pyproject.toml`                  | scikit-build-core backend + CMake options (repo root)       |
| `src/platform/python_bridge.cpp`  | the pybind11 module (the Python "host shim")                |
| `python/mirobody/__init__.py`     | thin Python wrapper re-exporting the compiled module        |
| CMake `MIROBODY_BUILD_PYTHON` opt | adds the `_mirobody` target (off by default; the wheel flips it on) |

## Prerequisites

The extension links the same native dependencies as the desktop build (curl,
OpenSSL, libwebsockets, yaml-cpp, hiredis, SQLite, …), resolved through **vcpkg**.
You therefore pass the vcpkg toolchain + triplet at build time, exactly as
`build.cmd` does for the desktop binary. You also need a C++ compiler toolchain
and CMake ≥ 3.19 on `PATH`.

## Building the wheel

### Windows (recommended): `build-python.cmd`

From the repo root, in any shell:

```
build-python.cmd
```

It mirrors `build.cmd`: runs `vcvarsall.bat` to bring up the MSVC + vcpkg
environment, forwards the toolchain/triplet, and writes the wheel to `dist\`.
Override `VS_DIR` / `VS_ARCH` / `VCPKG_ROOT` in the environment as needed. The
result is `dist\mirobody-<version>-cp3XX-cp3XX-win_amd64.whl`, where `<version>`
comes from the Git tag (see [Publishing](#publishing-to-pypi)).

Two things `build-python.cmd` pins down that bit us during bring-up, worth
keeping if you build by hand instead:

- **Force the Ninja generator** (`set CMAKE_GENERATOR=Ninja`, with the
  VS-bundled `ninja.exe` on `PATH`). Left to its heuristics, scikit-build-core
  picked `NMake Makefiles` and built **Debug** — linking the debug CRT
  (`ucrtbased.dll`) and `-d` vcpkg libs, which won't load against a release
  Python.
- **Force the build type**: `-DCMAKE_BUILD_TYPE=Release` passed via
  `SKBUILD_CMAKE_ARGS`.

### By hand / other platforms

`pip wheel` with the toolchain forwarded as a config setting. On Linux/macOS:

```bash
pip wheel . \
  -C cmake.args="-DCMAKE_TOOLCHAIN_FILE=$VCPKG_ROOT/scripts/buildsystems/vcpkg.cmake;-DVCPKG_TARGET_TRIPLET=x64-linux;-DCMAKE_BUILD_TYPE=Release"
```

scikit-build-core auto-installs `ninja` and `cmake` into the isolated build env,
so you don't need them on `PATH` — but you do need a C++ compiler and the vcpkg
deps. The wheel lands in the current directory.

### Verified

The wheel built and ran end-to-end on Windows (x64, CPython 3.12): the module
imports, the self-registering MCP tools/agents load, and `Server.start()` applies
the SQLite schema, binds the port (`GET /` → 200), and `stop()` tears down
cleanly.

### Editable / dev install

```powershell
pip install --no-build-isolation -e . `
  -C cmake.args="-DCMAKE_TOOLCHAIN_FILE=$tc;-DVCPKG_TARGET_TRIPLET=x64-windows-static-md"
```

## Using it

> **Self-contained wheel.** `build-python.cmd` builds with the
> `<arch>-windows-static-md` triplet, so the vcpkg deps (curl, OpenSSL,
> libwebsockets, …) are linked **into** `_mirobody.pyd` and the wheel needs no
> loose DLLs on `PATH`. (Linux/macOS already link these statically by default.)
> Just `import mirobody`.

```python
import mirobody

with mirobody.Server(listen_port=8080, openai_api_key="sk-...") as srv:
    print("listening on", srv.listen_port())
    # the HTTP/WebSocket front door is now up on 127.0.0.1:<port>
```

`Server` keyword arguments (all optional):
`config_path`, `data_dir` (SQLite file location), `sql_dir` (DDL directory),
`listen_addr`, `openai_api_key`, `gemini_api_key`, `listen_port`.

## Publishing to PyPI

The package is [`mirobody`](https://pypi.org/project/mirobody/) on PyPI. The
version is **derived from the Git tag** by [setuptools-scm](https://setuptools-scm.readthedocs.io/)
(wired through scikit-build-core in [`pyproject.toml`](../pyproject.toml)) — there
is no literal version in the tree to bump. To cut a release, tag and push:

```sh
git tag v2.0.0                         # release tags are vX.Y.Z
git push origin v2.0.0
```

A build from that exact commit reports `2.0.0`; an untagged build gets a PEP 440
dev version (`2.0.1.dev4+g<sha>`). `python/mirobody/__init__.py` reads the
installed version back via `importlib.metadata`.

Because the wheel is a **compiled extension**, each `build-python.*` run produces
a wheel for **only the current OS + CPython version** (e.g. `cp312-win_amd64`).
Ship a **source distribution** alongside it so platforms with no matching wheel
fall back to building from source:

```sh
# 1. platform wheel (needs the vcpkg toolchain — see above)
./build-python.sh                      # or build-python.cmd  ->  dist/mirobody-<version>-*.whl

# 2. source distribution (pure packaging, no compiler/vcpkg needed)
python -m build --sdist                # pip install build  ->  dist/mirobody-<version>.tar.gz

# 3. upload both to PyPI (needs a PyPI API token)
python -m twine upload dist/mirobody-*
```

The sdist carries `src/`, the self-registering `res/` agents + MCP tools,
`CMakeLists.txt`, and `vcpkg.json` (the host apps and web/native clients are
excluded via `tool.scikit-build.sdist` in `pyproject.toml`). A from-source
`pip install` still requires the user to have a C++ toolchain, CMake ≥ 3.19, and
the vcpkg deps available — so the wheel is the happy path; the sdist is the
fallback. For prebuilt wheels across Linux/macOS/Windows and multiple Python
versions, wire up [`cibuildwheel`](https://cibuildwheel.pypa.io/) in CI.

> A published version is **immutable** — PyPI never lets you re-upload
> `2.0.0` once it exists, even after deletion. Test against
> [TestPyPI](https://test.pypi.org/) (`twine upload --repository testpypi …`)
> first if unsure.

## Caveats

- **Native deps are linked statically.** `build-python.cmd` uses the
  `<arch>-windows-static-md` triplet (static libs, dynamic `/MD` CRT), so the
  vcpkg libraries are baked into `_mirobody.pyd` and the wheel is self-contained;
  Linux/macOS link them statically by default. The alternative — keeping the
  dynamic triplet and bundling the loose DLLs with
  [`delvewheel`](https://github.com/adang1345/delvewheel) (Windows),
  [`auditwheel`](https://github.com/pypa/auditwheel) (Linux), or
  [`delocate`](https://github.com/matthew-brett/delocate) (macOS) — is no longer
  needed here. For automated multi-platform wheels, wire up
  [`cibuildwheel`](https://cibuildwheel.pypa.io/).
- **Resources at runtime.** The server reads SQL DDL from `sql_dir` (`res/sql`
  by default) and an optional `config.yml`. These are **not** yet bundled into
  the wheel — point `sql_dir`/`config_path` at a checkout, or extend
  `tool.scikit-build` to ship `res/` and resolve it relative to the package.
- **Linux PIC.** Linking the static core into a shared module needs
  position-independent code; the CMake block sets `POSITION_INDEPENDENT_CODE` on
  our targets, but the vcpkg deps must also be PIC (use a PIC-enabled triplet).
- **API surface.** The wrapper currently covers server lifecycle
  (start/stop/is_running/listen_port). Richer in-process calls mean growing
  `python_bridge.cpp` (and, if you want the C ABI too, `src/mirobody.h`).
```

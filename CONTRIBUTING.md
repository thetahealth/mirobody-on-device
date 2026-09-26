# Contributing to mirobody-on-device

Thank you for helping. This repo is the phone runtime of
[mirobody](https://github.com/thetahealth/mirobody): a C++ core and the three
apps that embed it. Most of what makes a contribution land here, rather than
bounce, is knowing which of the two repos it belongs in.

## 🧭 Which repo

| The change | Goes to |
|---|---|
| A HealthKit / Health Connect / Huawei Health reader, a Bluetooth sensor, the app shells, the on-device model runtime, the C ABI, the loopback front door | **this repo** |
| A vendor field coded to the wrong LOINC code, in the shared crosswalk | [mirobody](https://github.com/thetahealth/mirobody) (`mirobody/res/crosswalks/`), then this repo loads it |
| Accounts, sharing, cloud vendor pulls, the server agent, the API contract, the SSE wire format | [mirobody](https://github.com/thetahealth/mirobody) |
| The chat UI and its rendering | [mirobody-web](https://github.com/thetahealth/mirobody-web) |

The test is: could a server do it? If yes, it belongs in the main repo, and the
phone gets it by speaking that repo's API. This repo was the full C++ port of
the server until September 2026, and a feature that grows back here is a
second implementation to keep in step with the first.

## 🐛 Reporting a bug

Open an issue. The templates ask for what a fix needs:

- **A reading coded wrong**
  ([template](https://github.com/thetahealth/mirobody-on-device/issues/new?template=wrong-reading.yml)):
  the health store, the field or type identifier exactly as that store names
  it, the value and unit, and what it was coded as.
- **Anything else**
  ([template](https://github.com/thetahealth/mirobody-on-device/issues/new?template=bug.yml)):
  the app and its OS version, the device, the commit, and the steps.

**Never paste real health data** into an issue, a log excerpt or a pull
request: not a screenshot of your readings, not an export. A synthetic value
that reproduces the problem is worth more to the fix than a real one, and it
is the only kind that can go in a test. A security problem does not go in an
issue at all: see [SECURITY.md](SECURITY.md).

To suggest a feature, open an issue first so the "which repo" question gets
settled before the code is written.

## 🛠️ Development workflow

1. **Fork and clone.**
   ```sh
   git clone https://github.com/YOUR_USERNAME/mirobody-on-device.git
   cd mirobody-on-device
   ```
   The dependencies for Linux, macOS and Windows are listed in
   [docs/BUILDING.md](docs/BUILDING.md).

2. **Branch.** `git checkout -b fix/short-name` (or `feat/`, `docs/`).

3. **Make the change**, following the style below.

4. **Run the gates.** They are the ones CI runs, on Ubuntu 24.04 and macOS 15:
   ```sh
   ./build.sh                       # the development build: core, loopback server, CLIs, tests
   build/tests/mirobody_tests       # the unit tests
   ./build.sh mobile                # the HarmonyOS profile: no HTTP front door, libraries only
   python3 tools/check_doc_links.py # every relative link in every tracked .md resolves
   python3 tools/check_exports.py   # the Windows export table matches src/mirobody.h
   ```
   `./build.sh mobile` is not optional for a change under `src/`: code that
   includes the front door from a file the phone profile also compiles builds
   fine on your desktop and breaks HarmonyOS.

   **The apps are not built in CI yet**: they need cross-compiled dependency
   sysroots. If your change touches `android/`, `ios/`, `harmony/`, the C ABI or
   `src/platform/`, build the affected app (its README says how) and say in the
   pull request which app you ran, on which device or emulator, and what you
   checked.

5. **Open a pull request** against `main`. The template asks which gates you
   ran; paste the test count line.

## 📝 Coding style

The rules this codebase converged on. Several are not the defaults you might
assume, so they are worth reading once.

### Priorities, in order

1. **Correct.** A confident wrong answer is the worst thing this project can
   produce: it is health data, and here it is the only copy.
2. **Testable.** Prefer a function that takes its inputs over one that reaches
   for global config. If it cannot be tested without a device, say why in the
   comment above it.
3. **Small.** Every byte ships inside an app. No abstraction built for one
   caller, no defensive checks for states that cannot occur, no
   compatibility shims. Delete dead code rather than commenting it out: `git`
   remembers, and the `v2-full-2026-08` tag keeps everything the refocus cut.

### C++

- **C++17**, set in [CMakeLists.txt](CMakeLists.txt) and enforced by the target
  feature plus the static assertion in `src/platform/log.hpp`. Use the standard
  library directly. Do not raise the floor to C++20 without checking the
  Android NDK, Apple Clang and HarmonyOS toolchains together.
- **A new third-party dependency costs four builds**: vcpkg on Windows, and the
  Android, iOS and HarmonyOS prebuilt sysroots. Propose it in an issue before
  adding it.
- **The C ABI in [`src/mirobody.h`](src/mirobody.h) is a contract** with three
  apps that ship on their own schedules. Add functions rather than changing
  one's meaning; a new function is also added to
  [`src/platform/mirobody.def`](src/platform/mirobody.def), which
  `tools/check_exports.py` enforces.
- **An MCP tool or an agent is one file.** Drop a `.cpp` into
  [`res/mcp_tools/`](res/mcp_tools/) (or `res/agents/`) that self-registers
  with `MIROBODY_REGISTER_TOOL`; `echo.cpp` is the example. No edit to
  `CMakeLists.txt` is needed.
- **Logs carry ids, counts, durations and status codes. Never a value.** A
  reading, a note, a message body or an indicator name in a log line is health
  data outside the record: in logcat, in a crash report, in the log excerpt
  someone pastes into an issue.
- **Line endings are per file.** `.gitattributes` pins `*.sh` to LF and
  `*.cmd` to CRLF, and some Markdown files are CRLF. Keep whatever the file
  already uses, so a one-line change is not a whole-file diff.

### Comments

**English, always**, including a comment that quotes non-English data.
**Explain why, with evidence; never restate the code.** A comment naming the
failure it prevents survives a refactor; one that paraphrases the line becomes
a lie the first time the line changes.

```cpp
// Bad: says what the next line already says
// Set the listen address
cfg.listen_addr = "127.0.0.1";

// Good: records the bug that motivated the line
// A caller passing 0.0.0.0 is asking for the default, not for the LAN: the
// embedded server used to bind every interface and served the health record
// to anyone on the same Wi-Fi.
if (cfg.listen_addr.empty() || cfg.listen_addr == "0.0.0.0") cfg.listen_addr = "127.0.0.1";
```

A stale comment is a bug. If you change behaviour, the comments describing it
are part of the change.

### Verify before you assert

**Do not reason from what the code appears to do: run it.** Before deleting
something as unused, check who calls it transitively, including the three apps
across the C ABI and JNI. Before repeating a claim from a README, run the
command. The refocus found a README describing a consent page that had been
deleted with the web client.

### Documentation

Docs are code. A command in a README is a promise that it runs, and a link is
a promise that it resolves (`tools/check_doc_links.py` holds the second one).
If you rename or delete a file, grep the `.md` files. Which document owns
what is in [docs/README.md](docs/README.md).

The top-level README ships in English and Chinese. **English is the source of
truth**: change it first, then [README.zh-CN.md](README.zh-CN.md). If you
change only the English, say so in the pull request so the drift is visible.
Everything else stays English.

### Commits

A subject of at most 50 characters with an area prefix, the way the history
reads: `Platform: bind the embedded Android server to loopback`, `Health: …`,
`Clients: …`, `Docs: …`. The body says **why**, and how the change was
verified. "fix bug" tells a future reader nothing; "the embedded server bound
0.0.0.0 and answered on the LAN" tells them everything.

User-visible changes get a line under **Unreleased** in
[CHANGELOG.md](CHANGELOG.md), written as what was wrong, what changed, and how
to tell.

## ⚖️ License

By contributing, you agree that your contributions are licensed under the
project's [Apache 2.0 license](LICENSE).

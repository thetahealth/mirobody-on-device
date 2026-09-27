# Working on mirobody-on-device with a coding agent

Read this before touching the tree. It is the short version of
[CONTRIBUTING.md](CONTRIBUTING.md), plus the things an agent gets wrong first.

## What this is

The phone runtime of [mirobody](https://github.com/thetahealth/mirobody): a
C++17 core embedded by HarmonyOS and optionally by Android and iOS, plus
those apps.

| Part | Where | Notes |
|---|---|---|
| The public C ABI | `src/mirobody.h` | host integration contract; mirrored in `src/platform/mirobody.def`. Android and iOS still use HTTP for most app operations |
| Platform bridges | `src/platform/` (`android_jni.cpp`, `ios_bridge.mm`, `c_api*.cpp`), `harmony/entry/src/main/cpp/napi_init.cpp` | JNI, iOS, NAPI |
| Agent loop, model clients, tools | `src/chat/`, `src/llm/`, `src/mcp/`, `res/mcp_tools/`, `res/agents/` | tools and agents self-register, one file each |
| The record | `src/fhir/`, `src/health/timeseries/` (health-store ingest), `src/database/` (SQLite only), `src/storage/` (local files only) | |
| Loopback front door | `src/server/` | off in the `MIROBODY_MOBILE` profile (HarmonyOS) |
| Apps | `android/`, `ios/`, `harmony/` | Kotlin / Compose, SwiftUI, ArkTS |

Do not build on these. `src/user/`, `src/circle/`, `src/oauth/` and `src/jwt/`
are scheduled for removal once the apps move to a per-launch token. EHR (SMART on
FHIR, `src/health/ehr_connect.*`) is out of scope for now, so do not extend it.

## The gates: run them before you say "done"

```sh
./build.sh && build/tests/mirobody_tests   # development build + unit tests
./build.sh mobile                          # the HarmonyOS profile, no front door
python3 tools/check_doc_links.py           # relative links in every tracked .md
python3 tools/check_exports.py             # mirobody.def matches mirobody.h
```

CI runs these on Ubuntu 24.04 and macOS 15. A separate workflow runs Android
JVM tests and builds/runs the pure-client iOS app on a simulator. Neither
workflow verifies native embedded app builds. If you touched an app,
`src/platform/` or the C ABI and could not build the affected native path, say
so; do not report it as verified.

## Rules that are not the defaults you would assume

- **Does a server need it? Then it goes in the main repo, not here.** No
  Postgres, no object stores, no Redis, no cloud vendor connector, no second
  server agent. They were cut in September 2026 and preserved at the
  `v2-full-2026-08` tag. Do not restore them from there.
- **C++17.** Use the standard library directly. Do not add a compatibility
  shim for features already covered by the project floor. C++20 is not required.
- **No new third-party dependency without an issue first.** Each one has to be
  cross-built into three phone sysroots and vcpkg.
- **The C ABI is append-only in meaning.** Add a function rather than change what
  one does; the three apps ship on their own schedules.
- **Embedded listeners bind `127.0.0.1`.** Never widen them, not even for a
  test. The standalone development process accepts an explicit `HTTP_HOST`
  override; do not carry that behavior into the phone bridges.
- **Logs carry ids, counts, durations, status codes. Never a value**, and never
  an indicator name.
- **Preserve each file's line endings.** Some `.md` files and `.gitignore` are
  CRLF; `*.cmd` must be CRLF and `*.sh` LF (`.gitattributes`). Tools that
  rewrite a file in text mode convert CRLF silently. Check with `git diff --stat`
  that a one-line edit is a one-line diff.
- **`*.yml` is gitignored.** A new YAML file under `.github/` needs its own
  negation in `.gitignore`, or it is silently not committed.
- **Comments say why, with evidence; never what.** English only. A stale comment
  is a bug and part of your change.
- **Verify, don't reason.** Before deleting "unused" code, check callers
  transitively, including the apps (Kotlin, Swift, ArkTS) across the C ABI.
  Before repeating a README claim, run the command.
- **Docs are code.** Rename or delete a file, then grep the `.md` files. The
  README ships in English and Chinese: English first, then `README.zh-CN.md`.

## Never commit

- `config.yml`, `.env`, anything under `_local/`, and anything holding a key.
- Real health data: a reading, an export, a screenshot of one, a log excerpt
  with values in it.
- `internal/`: gitignored planning material, and nothing there is a promise.

## Where things are decided

- [CHANGELOG.md](CHANGELOG.md): **Unreleased** gets an entry for every
  user-visible change.
- [docs/README.md](docs/README.md): which document owns what.
- [docs/privacy-tiers.md](docs/privacy-tiers.md): target lanes and current
  host-specific data flows. A change that sends something new off the phone
  changes this file too.
- The main repo owns the API contract, the SSE wire format and the terminology.
  Match it; do not fork it.

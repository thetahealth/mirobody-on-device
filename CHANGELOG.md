# Changelog

Every user-visible change, newest first, written as what was wrong, what
changed, and how to tell. The repo has no numbered releases yet; entries are
dated by the day they reached `main`.

## Unreleased

### Changed

- Added Android phone-flavor JVM tests and pure-client iOS simulator tests to
  CI. Regenerated the checked-in Xcode project so its Test action actually runs
  the existing `MirobodyTests` target; the prior project silently omitted it.
- Clarified the contributor and agent guides after the C++17 migration: the
  native registration macros, unconditional embedded loopback binding, and
  current Android/iOS integration paths now match the code. Added a
  [testing guide](docs/testing.md) that separates CI coverage from phone app
  builds and records the checks required for pull requests.
- Protected `main` with a PR requirement and the Ubuntu and macOS core checks;
  blocked force pushes, deletion and unresolved review threads. Approval
  count is zero while the project has no guaranteed independent reviewer.
- The shared core now requires C++17. The custom `optional` compatibility layer
  is gone, the host-facing C ABI is unchanged, and the build guide documents the
  Android, Apple and OpenHarmony toolchain floor. Rebuild native libraries and
  XCFrameworks against the new core: internal C++ APIs changed even though the
  C ABI contract did not.
- Added [architecture.md](docs/architecture.md), which defines the host/core
  boundary, local/BYOK/server lanes and the mobile-first build profiles. The
  English and Chinese READMEs now describe this repository as a phone runtime
  instead of a second server.
- Embedded Android and iOS servers now bind loopback even when a config file or
  `HTTP_HOST` requests a LAN address. The standalone development process still
  accepts an explicit host override.

### Added

- The project files the main repo has and this one did not:
  [CONTRIBUTING.md](CONTRIBUTING.md) (which repo a change belongs in, the gates,
  the C++17 and C ABI rules), [SECURITY.md](SECURITY.md) (scoped to what a phone
  runtime exposes: the loopback front door, other apps, the lanes),
  [AGENTS.md](AGENTS.md), the [Code of Conduct](CODE_OF_CONDUCT.md), issue
  forms for a wrongly coded reading and for everything else, a pull-request
  template, and [docs/README.md](docs/README.md), which says which document owns
  what.
- The README in Chinese, [README.zh-CN.md](README.zh-CN.md). The English README
  is restructured the way the main repo's is, and leads with a two-minute
  desktop run whose commands and output were checked.
- Two checks, both run by CI: `tools/check_doc_links.py` fails on a relative
  link in a tracked `.md` file that points at no file or no heading, and
  `tools/check_exports.py` fails when the Windows export table and
  `src/mirobody.h` disagree.

### Fixed

- The Windows DLL exported 12 of the 24 functions in `src/mirobody.h`. A
  Windows host could start the core and chat, but could not reach
  `mirobody_chat_answer*`, the health store (`mirobody_health_store`,
  `mirobody_health_recent`) or the on-device model (`mirobody_llm_*`).
  `src/platform/mirobody.def` now lists all 24, and `tools/check_exports.py`
  keeps it that way. (No CI job links on Windows, so this is checked by
  comparing the two files, not by a Windows build.)
- Seven links pointed at README sections the refocus deleted, or at a
  `config.yml` that is not tracked. They now point at the current file, the
  main repo, or the archived v2 README.

### Removed

- `docs/mirobody-v2.md`, the v2 architecture deck. Nothing linked to it, and it
  described Electron, Qt, the Python wheel and S3 as current. It is at the
  `v2-full-2026-08` tag.

## 2026-09-26: the phone runtime

This repo was the full C++ port of an earlier mirobody server ("mirobody v2").
It now holds only what needs a phone
([#1](https://github.com/thetahealth/mirobody-on-device/pull/1)). Everything cut
is preserved at the `v2-full-2026-08` tag and the `archive/v2-full` branch.

### Security

- The embedded Android server bound every interface when the app asked for
  `0.0.0.0`, which served the health record to anyone on the same network. It
  now binds `127.0.0.1` whether the address is empty or `0.0.0.0`; from another
  machine on the LAN, the port does not answer.
- At startup the core fetched 23 third-party vendor favicons, telling each of
  those hosts that the app was running. It fetches nothing now.

### Removed

- The desktop and web clients (Electron, Qt, `htdoc/`, the WeChat mini
  program), the Python wheel and language bindings, and the Docker image.
- The server databases (Postgres, MySQL, ClickHouse, DuckDB), the object stores
  (S3, OSS, Azure Blob), Redis, the hosted memory services (Mem0, Zep), the
  realtime voice lanes, and 23 cloud vendor connectors.

### Changed

- SQLite is the only database and the local filesystem the only file store.
- The core static library went from 7.77 MB to 5.62 MB (development profile,
  macOS arm64).
- `config.example.yml` holds only keys the core reads, with the front door on
  `127.0.0.1`.
- CI builds the core and runs the unit tests on Ubuntu 24.04 and macOS 15, and
  builds the HarmonyOS profile.

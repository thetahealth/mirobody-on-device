# Testing changes

The desktop build is the test harness for the shared C++17 core. It is not a
phone app build. Run the checks that match your change and describe the result
in the pull request. The `ci` workflow runs every command in the first table
on both Ubuntu 24.04 and macOS 15; `host-tests` runs the pure-client app tests
below.

| Check | Command | What it establishes |
|---|---|---|
| Development profile | `./build.sh` | Core, desktop front door, CLIs and test binary compile on the host. |
| Unit tests | `build/tests/mirobody_tests` | The checked-in C++ tests pass; report the pass and skip counts. |
| Mobile profile | `./build.sh mobile` | The core compiles without the HTTP front door on the host. This does **not** cross-build a HarmonyOS app. |
| Documentation | `python3 tools/check_doc_links.py` | Tracked Markdown links point to existing local files and headings. |
| C ABI exports | `python3 tools/check_exports.py` | The Windows export list names the functions declared in `src/mirobody.h`. This does **not** link a Windows DLL. |

The CI jobs are named `core (ubuntu-24.04)` and `core (macos-15)`. The active
[main rule](https://github.com/thetahealth/mirobody-on-device/rules/24043212)
requires both on pull requests targeting `main`. A docs-only PR can run the link
checker locally; CI still exercises the complete matrix before merge.

## Phone host verification

| CI job | Command | What it establishes |
|---|---|---|
| `android JVM (pure client)` on Ubuntu 24.04 | `gradle -p android :app:testPhoneDebugUnitTest -Pmirobody.native=false --no-daemon --console=plain` (Gradle 9.4.1, JDK 21) | Kotlin phone-flavor logic compiles and its JVM tests pass. No APK, JNI or Android device runs. |
| `iOS simulator (pure client)` on macOS 15 | `xcodebuild test -project ios/Mirobody.xcodeproj -scheme Mirobody -destination "platform=iOS Simulator,id=<available-iPhone-UDID>" CODE_SIGNING_ALLOWED=NO` | The checked-in project builds a simulator app and runs `MirobodyTests`. No XCFramework, physical device or signed app is tested. |

The checked-in Xcode project must contain the `MirobodyTests` target and the
scheme's Test action must include it. After editing `ios/project.yml`, run
`cd ios && xcodegen generate` and commit the generated project too.

| Changed area | Additional evidence to seek | Starting point |
|---|---|---|
| `android/`, Android JNI or the C ABI | Build the selected Android ABI, launch the app and exercise the changed path. | [Android guide](../android/README.md) |
| `ios/`, iOS bridge or the C ABI | Build the app or XCFramework configuration affected, then exercise the changed path. | [iOS guide](../ios/README.md) |
| `harmony/`, NAPI or the C ABI | Build the HAP/native module and exercise the changed path. | [HarmonyOS guide](../harmony/README.md) |
| `src/` shared behavior | Run both desktop profiles and tests; add a host run when the change alters a platform boundary. | [Architecture](architecture.md) |

For Android host logic, run
`cd android && ./build-app.sh :app:testPhoneDebugUnitTest` on macOS/Linux,
or the same task with `build-app.cmd` on Windows. The existing JVM tests cover
Bluetooth coding, local model selection and chat rendering. For iOS, use the
Mirobody scheme's **Test** action in Xcode; `MirobodyTests` currently covers
GATT decoding. Report Android JVM tests separately from a device or emulator
run.

The native dependency sysroots are not built in CI. If you cannot build an
affected native app path, write **“Native app build not verified”** in the PR,
with the missing sysroot or toolchain and the host path affected. A desktop
core pass or pure-client test must not be reported as verification of an
embedded Android, iOS or HarmonyOS build. A source inspection of a bridge is
useful evidence, but it is not an app run.

Use synthetic health readings and files in tests and issues. Do not commit a
real reading, export, screenshot or log value. For a wrong code or unit, name
the health-store type and provide a synthetic value that reproduces the
mapping. See [CONTRIBUTING.md](../CONTRIBUTING.md) for repository ownership and
[SECURITY.md](../SECURITY.md) for private vulnerability reports.

## Pull request evidence

State the command, exit result and test count. Include the selected backend
and model lane when testing a phone data path; local model execution alone does
not establish local record storage. If a change sends a new artifact off the
phone, update [privacy-tiers.md](privacy-tiers.md) in the same PR.

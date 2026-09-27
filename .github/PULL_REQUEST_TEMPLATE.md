## What this changes

<!-- One or two sentences, and why. If it fixes an issue, "Fixes #NN". -->

## Which gates you ran

<!-- Paste the test count line. CI runs the first four on Ubuntu and macOS. -->

- [ ] `./build.sh && build/tests/mirobody_tests`
- [ ] `./build.sh mobile`: required for anything under `src/`
- [ ] `python3 tools/check_doc_links.py`
- [ ] `python3 tools/check_exports.py`: required when `src/mirobody.h` changes

## Which app you ran it in

<!-- CI runs Android JVM tests and pure-client iOS simulator tests, but no
     embedded native app build. If this touches android/, ios/, harmony/,
     src/platform/ or the C ABI: which app, device/emulator and path did you
     check? If you could not build the native path, write "Native app build not
     verified" and name the missing sysroot/toolchain. "Core only" is fine
     for unrelated changes. See docs/testing.md for the host matrix. -->

## Data boundary, if affected

<!-- State the selected backend and model lane you tested. If a new artifact
     can leave the phone, update docs/privacy-tiers.md in this PR. Otherwise,
     write "No data-boundary change". -->

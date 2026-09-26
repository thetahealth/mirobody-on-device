## What this changes

<!-- One or two sentences, and why. If it fixes an issue, "Fixes #NN". -->

## Which gates you ran

<!-- Paste the test count line. CI runs the first four on Ubuntu and macOS. -->

- [ ] `./build.sh && build/tests/mirobody_tests`
- [ ] `./build.sh mobile`: required for anything under `src/`
- [ ] `python3 tools/check_doc_links.py`
- [ ] `python3 tools/check_exports.py`: required when `src/mirobody.h` changes

## Which app you ran it in

<!-- CI builds no app yet. If this touches android/, ios/, harmony/, src/platform/
     or the C ABI: which app, which device or emulator, and what you checked.
     "Core only" is a fine answer for everything else. -->

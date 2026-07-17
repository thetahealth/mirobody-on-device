#!/usr/bin/env bash
# build-electron.sh -- build + run/package the Mirobody Electron desktop app (Linux/macOS).
#
# Electron-focused: it builds the web bundle (res/htdoc), installs the Electron deps
# (electron, electron-builder, koffi, node-llama-cpp), then runs or packages. It does
# NOT build the C++ shared library -- that needs the full toolchain; run ../build-shared.sh
# first (once) if build-shared/libmirobody.{so,dylib} is missing.
#
# Usage: ./build-electron.sh [run|dist] [clean]
#   run    (default)  -> npm start   (dev: embed the server, open the window)
#   dist              -> npm run dist (electron-builder package)
#   clean             -> wipe electron/node_modules before installing
#   -h/--help         -> show this help
set -euo pipefail

ELECTRON_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "$ELECTRON_DIR/.." && pwd)"

MODE=run ; CLEAN=""
for tok in "$@"; do
    case "$tok" in
        run)  MODE=run ;;
        dist) MODE=dist ;;
        clean) CLEAN=1 ;;
        -h|--help) sed -n '2,14p' "${BASH_SOURCE[0]}" | sed 's/^# \{0,1\}//'; exit 0 ;;
        *) echo "[build-electron] Unknown token: $tok" >&2; exit 2 ;;
    esac
done

# 1. C++ shared library (embedded server, loaded via koffi) -- must be prebuilt.
if [ ! -f "$ROOT/build-shared/libmirobody.so" ] && [ ! -f "$ROOT/build-shared/libmirobody.dylib" ]; then
    echo "[build-electron] Missing build-shared/libmirobody.{so,dylib}." >&2
    echo "                 Build it once first: ../build-shared.sh" >&2
    exit 1
fi

# 2. Web bundle (htdoc -> res/htdoc), the renderer Electron loads.
echo "[build-electron] Building web bundle (htdoc)..."
( cd "$ROOT/htdoc" && npm install && npm run build )

# 3. Electron deps.
[ -n "$CLEAN" ] && rm -rf "$ELECTRON_DIR/node_modules"
cd "$ELECTRON_DIR"
if [ ! -d node_modules ]; then
    echo "[build-electron] Installing Electron deps..."
    npm install
fi

# 4. Run / package.
if [ "$MODE" = dist ]; then
    echo "[build-electron] Packaging (electron-builder)..."
    npm run dist
else
    echo "[build-electron] Launching (npm start)..."
    npm start
fi

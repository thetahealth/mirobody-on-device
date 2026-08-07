# Generate Mirobody app icons for HarmonyOS: the product brand mark on a white tile.
#
# The mark itself lives in tools/brandmark.py, shared with the other clients' icon
# generators -- Harmony was once the last client drawing something else (a hand-plotted
# heartbeat pulse), so HarmonyOS users saw a different logo in the launcher than
# everyone else did. Keeping the geometry in one module is what stops that recurring.
import os
import sys

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", ".."))
from tools.brandmark import make_tile, make_foreground, solid_white   # noqa: E402

BASE = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..")
APPSCOPE = os.path.join(BASE, "AppScope/resources/base/media")
ENTRY = os.path.join(BASE, "entry/src/main/resources/base/media")

SIZE = 1024
make_tile(SIZE).save(os.path.join(APPSCOPE, "app_icon.png"))
solid_white(SIZE).save(os.path.join(ENTRY, "background.png"))     # full-bleed, system masks
make_foreground(SIZE).save(os.path.join(ENTRY, "foreground.png"))
make_tile(SIZE).save(os.path.join(ENTRY, "startIcon.png"))
print("brand-mark icons written")

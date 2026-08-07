# Generate the Mirobody desktop icon assets for the Qt client.
#
# Two files, because "the app's icon" is two different things on a desktop:
#
#   mirobody.ico  the EXECUTABLE's icon -- what Explorer, the Start menu, and the
#                 taskbar's pinned entry show. Windows reads it from a resource
#                 compiled into the .exe (mirobody.rc), never from a loose file, so
#                 it has to be a real multi-frame .ico.
#   mirobody.png  the WINDOW's icon -- QGuiApplication::setWindowIcon, embedded in
#                 the QML module's resources. This is the one that matters on Linux
#                 (an ELF has no icon resource) and for the title bar / alt-tab.
#                 PNG rather than reusing the .ico because the PNG handler is built
#                 into QtGui while ICO is a deployable plugin.
#
# The mark comes from tools/brandmark.py, same geometry and same white rounded tile
# as the HarmonyOS launcher icon and android's adaptive icon -- so all three clients
# show one logo.
#
# Run after changing tools/brandmark.py; the outputs are committed:
#   python qt/icon/gen_icon.py     # needs Pillow (PIL)
import os
import sys

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", ".."))
from tools.brandmark import make_tile   # noqa: E402

OUT = os.path.dirname(os.path.abspath(__file__))

# The frame set Windows actually asks for. 256 is the "large icons" view, 16 the
# tree/title bar; the ones between are what shell views and the taskbar pick from.
ICO_SIZES = [(16, 16), (24, 24), (32, 32), (48, 48), (64, 64), (128, 128), (256, 256)]

# Rendered once at the largest frame. make_tile() already draws at 4x and
# Lanczos-downsamples, so letting PIL derive the smaller frames from this costs
# nothing that re-rendering each size would recover.
tile = make_tile(256)
tile.save(os.path.join(OUT, "mirobody.ico"), sizes=ICO_SIZES)
tile.save(os.path.join(OUT, "mirobody.png"))
print("wrote mirobody.ico (%d frames) + mirobody.png" % len(ICO_SIZES))

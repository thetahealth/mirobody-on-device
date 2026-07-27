# Generate Mirobody app icons: flat, line-style — white tile + a brand-blue heartbeat pulse
# stroke (no gradient, no fill, no shadow). Line color picked from the app palette (#3F7FC1).
from PIL import Image, ImageDraw
import os

BASE = r"d:/projects/mirobody-v2/harmony"
APPSCOPE = os.path.join(BASE, "AppScope/resources/base/media")
ENTRY = os.path.join(BASE, "entry/src/main/resources/base/media")

WHITE = (255, 255, 255, 255)
LINE = (63, 127, 193, 255)   # #3F7FC1 brand blue (from the app palette)

def rounded_white(size, radius_ratio=0.22):
    img = Image.new("RGBA", (size, size), (0, 0, 0, 0))
    d = ImageDraw.Draw(img)
    d.rounded_rectangle([0, 0, size - 1, size - 1], radius=int(size * radius_ratio), fill=WHITE)
    return img

def pulse_points(size):
    s = size / 1024.0
    def P(x, y): return (int(x * s), int(y * s))
    return [P(250, 560), P(405, 560), P(458, 560),
            P(502, 430), P(548, 704), P(592, 356), P(636, 648), P(682, 560),
            P(774, 560)]

def draw_pulse(img, size, width_ratio=0.052):
    d = ImageDraw.Draw(img)
    pts = pulse_points(size)
    w = max(2, int(size * width_ratio))
    d.line(pts, fill=LINE, width=w, joint="curve")
    r = w // 2
    for (x, y) in [pts[0], pts[-1]]:          # round caps on the ends
        d.ellipse([x - r, y - r, x + r, y + r], fill=LINE)

def make_tile(size):
    """Flat line icon: white rounded tile + blue pulse stroke."""
    img = rounded_white(size)
    draw_pulse(img, size)
    return img

def make_foreground(size):
    """Layered-icon foreground: transparent + blue pulse (system masks the shape)."""
    fg = Image.new("RGBA", (size, size), (0, 0, 0, 0))
    draw_pulse(fg, size)
    return fg

def solid_white(size):
    return Image.new("RGBA", (size, size), WHITE)

SIZE = 1024
make_tile(SIZE).save(os.path.join(APPSCOPE, "app_icon.png"))
solid_white(SIZE).save(os.path.join(ENTRY, "background.png"))     # full-bleed, system masks
make_foreground(SIZE).save(os.path.join(ENTRY, "foreground.png"))
make_tile(SIZE).save(os.path.join(ENTRY, "startIcon.png"))
print("flat line icons written")

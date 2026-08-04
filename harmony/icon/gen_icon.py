# Generate Mirobody app icons for HarmonyOS: the product brand mark on a white tile.
#
# The mark is the SAME two-lobe glyph android and the web client use -- paths copied
# verbatim from htdoc/src/assets/mirobody.svg (the single source; see
# docs/colors-and-fonts.md S5). Harmony was the last client still drawing something
# else (a hand-plotted heartbeat pulse), so HarmonyOS users saw a different logo in
# the launcher than everyone else did.
#
# Colors are FIXED here, not theme-aware: a launcher icon has no theme. The upper
# lobe takes brand_logo_mark's light value, the lower one the Theta Health blue that
# never varies by mode. (android's launcher foreground hardcodes #000000 for the
# upper lobe rather than #0F1115 -- invisible in practice, but the sheet's value is
# the one worth copying.)
#
# SVG paths are flattened here rather than handed to an external rasterizer so the
# script keeps needing nothing but PIL. Only the commands these two paths actually
# use are supported -- absolute M/C/H/V/Z; anything else raises rather than misdraw.
from PIL import Image, ImageDraw
import os
import re

BASE = r"d:/projects/mirobody-v2/harmony"
APPSCOPE = os.path.join(BASE, "AppScope/resources/base/media")
ENTRY = os.path.join(BASE, "entry/src/main/resources/base/media")

WHITE = (255, 255, 255, 255)
MARK_UPPER = (0x0F, 0x11, 0x15, 255)   # #0F1115  brand_logo_mark, light value
MARK_LOWER = (0x00, 0x5C, 0xF5, 255)   # #005CF5  fixed Theta Health blue

# htdoc/src/assets/mirobody.svg, viewBox 0 0 23 24. Keep identical to the source.
VIEWBOX = (23.0, 24.0)
PATH_UPPER = ("M22.8429 6.84462C22.8429 3.0773 19.9346 0.0197148 16.3398 0H0V10.3126"
              "H16.3398C17.9718 10.3216 19.4609 10.9579 20.6013 12.0009C21.9741 "
              "10.7464 22.8429 8.90213 22.8429 6.84462Z")
PATH_LOWER = ("M20.6013 12.0018C19.4609 13.0449 17.9718 13.6811 16.3398 13.6883H0"
              "V24.0009H16.3398C19.9346 23.9812 22.8429 20.9236 22.8429 17.1563"
              "C22.8429 15.0988 21.9741 13.2546 20.6013 12V12.0018Z")

# Mark size as a fraction of the canvas, matching android's adaptive icon exactly:
# ic_launcher_foreground.xml scales the 23x24 mark by 2.5 inside a 108 canvas and
# centers it, i.e. 60/108 of the height.
MARK_HEIGHT_RATIO = 60.0 / 108.0
SS = 4            # supersample factor; PIL's polygon fill is not antialiased


def parse_path(d):
    """Flatten an SVG path to a point list. Absolute M/C/H/V/Z only."""
    toks = re.findall(r"[A-Za-z]|-?\d*\.?\d+", d)
    pts, i, cur = [], 0, (0.0, 0.0)
    while i < len(toks):
        cmd = toks[i]
        if not re.match(r"^[A-Za-z]$", cmd):
            raise ValueError("expected a command at token %d, got %r" % (i, cmd))
        if cmd not in "MCHVZ":
            raise ValueError("unsupported command %r (absolute M/C/H/V/Z only)" % cmd)
        i += 1
        if cmd == "Z":
            continue                       # the polygon fill closes the ring itself
        if cmd == "M":
            cur = (float(toks[i]), float(toks[i + 1])); i += 2
            pts.append(cur)
        elif cmd == "H":
            cur = (float(toks[i]), cur[1]); i += 1
            pts.append(cur)
        elif cmd == "V":
            cur = (cur[0], float(toks[i])); i += 1
            pts.append(cur)
        elif cmd == "C":
            p0 = cur
            p1 = (float(toks[i]),     float(toks[i + 1]))
            p2 = (float(toks[i + 2]), float(toks[i + 3]))
            p3 = (float(toks[i + 4]), float(toks[i + 5]))
            i += 6
            for s in range(1, 25):         # 24 segments is smooth at 4x supersample
                t = s / 24.0
                u = 1.0 - t
                pts.append((
                    u*u*u*p0[0] + 3*u*u*t*p1[0] + 3*u*t*t*p2[0] + t*t*t*p3[0],
                    u*u*u*p0[1] + 3*u*u*t*p1[1] + 3*u*t*t*p2[1] + t*t*t*p3[1],
                ))
            cur = p3
    return pts


def draw_mark(img, size):
    """Fill both lobes of the brand mark, centered, at android's proportions."""
    vw, vh = VIEWBOX
    h = size * MARK_HEIGHT_RATIO
    scale = h / vh
    ox, oy = (size - vw * scale) / 2.0, (size - h) / 2.0
    d = ImageDraw.Draw(img)
    for path, color in ((PATH_LOWER, MARK_LOWER), (PATH_UPPER, MARK_UPPER)):
        d.polygon([(ox + x * scale, oy + y * scale) for (x, y) in parse_path(path)],
                  fill=color)


def rounded_white(size, radius_ratio=0.22):
    img = Image.new("RGBA", (size, size), (0, 0, 0, 0))
    ImageDraw.Draw(img).rounded_rectangle(
        [0, 0, size - 1, size - 1], radius=int(size * radius_ratio), fill=WHITE)
    return img


def supersampled(size, build):
    """Render at SS x and downsample -- the only antialiasing a polygon fill gets."""
    return build(size * SS).resize((size, size), Image.LANCZOS)


def make_tile(size):
    """Flat icon: white rounded tile + the brand mark."""
    def build(s):
        img = rounded_white(s)
        draw_mark(img, s)
        return img
    return supersampled(size, build)


def make_foreground(size):
    """Layered-icon foreground: transparent + the mark (the system masks the shape)."""
    def build(s):
        fg = Image.new("RGBA", (s, s), (0, 0, 0, 0))
        draw_mark(fg, s)
        return fg
    return supersampled(size, build)


def solid_white(size):
    return Image.new("RGBA", (size, size), WHITE)


SIZE = 1024
make_tile(SIZE).save(os.path.join(APPSCOPE, "app_icon.png"))
solid_white(SIZE).save(os.path.join(ENTRY, "background.png"))     # full-bleed, system masks
make_foreground(SIZE).save(os.path.join(ENTRY, "foreground.png"))
make_tile(SIZE).save(os.path.join(ENTRY, "startIcon.png"))
print("brand-mark icons written")

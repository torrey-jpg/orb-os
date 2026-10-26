#!/usr/bin/env python3
"""Kevin's Orb - the Weather screen's icons, drawn here and compiled into the firmware.

Writes src/kwx_icons.c: every icon at two sizes (a large one for "now", a small one for the
hourly strip) as LVGL 8 images, true colour with alpha (RGB565 + an alpha byte, the build's
LV_COLOR_DEPTH 16 with LV_COLOR_16_SWAP 0). Also writes a preview sheet.

The palette is chosen for red/green colour blindness: yellow sun, pale moon, grey clouds,
blue rain, yellow lightning, white snow. None of them depends on telling red from green,
and the condition is always written out in words beside the icon as well.

Usage: python3 tools/weather/make_icons.py [--preview sheet.png]
"""
import argparse, math, os
from PIL import Image, ImageDraw, ImageFilter

SS = 4                     # supersampling
ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))

SUN    = (255, 200, 61)
SUN_HI = (255, 228, 140)
MOON   = (233, 228, 207)
CLOUD  = (221, 227, 234)
CLOUD_SH = (176, 186, 198)
CLOUD_DARK = (150, 160, 173)
CLOUD_DARK_SH = (112, 122, 136)
RAIN   = (91, 192, 255)
BOLT   = (255, 212, 71)
SNOW   = (245, 248, 252)
FOG    = (174, 182, 191)

def canvas(n):
    return Image.new("RGBA", (n * SS, n * SS), (0, 0, 0, 0))

def sun(img, cx, cy, r):
    d = ImageDraw.Draw(img)
    s = img.size[0] / 100.0
    for k in range(8):
        a = k * math.pi / 4
        r0, r1, w = r * 1.35, r * 1.85, r * 0.16
        x0, y0 = cx + r0 * math.cos(a), cy + r0 * math.sin(a)
        x1, y1 = cx + r1 * math.cos(a), cy + r1 * math.sin(a)
        d.line([(x0 * s, y0 * s), (x1 * s, y1 * s)], fill=SUN, width=int(w * 2 * s))
        for (px, py) in ((x0, y0), (x1, y1)):
            d.ellipse([(px - w) * s, (py - w) * s, (px + w) * s, (py + w) * s], fill=SUN)
    d.ellipse([(cx - r) * s, (cy - r) * s, (cx + r) * s, (cy + r) * s], fill=SUN)
    rr = r * 0.62
    hl = Image.new("RGBA", img.size, (0, 0, 0, 0))
    ImageDraw.Draw(hl).ellipse([(cx - r * 0.35 - rr) * s, (cy - r * 0.35 - rr) * s,
                                (cx - r * 0.35 + rr) * s, (cy - r * 0.35 + rr) * s], fill=SUN_HI + (150,))
    hl = hl.filter(ImageFilter.GaussianBlur(r * 0.25 * s))
    mask = Image.new("L", img.size, 0)
    ImageDraw.Draw(mask).ellipse([(cx - r) * s, (cy - r) * s, (cx + r) * s, (cy + r) * s], fill=255)
    img.paste(Image.alpha_composite(img, hl), (0, 0), mask)

def moon(img, cx, cy, r):
    s = img.size[0] / 100.0
    m = Image.new("L", img.size, 0)
    ImageDraw.Draw(m).ellipse([(cx - r) * s, (cy - r) * s, (cx + r) * s, (cy + r) * s], fill=255)
    ImageDraw.Draw(m).ellipse([(cx - r * 0.35) * s, (cy - r * 1.15) * s, (cx + r * 1.65) * s, (cy + r * 0.85) * s], fill=0)
    layer = Image.new("RGBA", img.size, MOON + (255,))
    img.paste(layer, (0, 0), m)

def cloud(img, cx, cy, w, col=CLOUD, sh=CLOUD_SH):
    """A cloud w wide centred at (cx, cy), in 0..100 units."""
    s = img.size[0] / 100.0
    h = w * 0.62
    parts = [(cx - w * 0.22, cy + h * 0.05, w * 0.24), (cx + w * 0.05, cy - h * 0.14, w * 0.30),
             (cx + w * 0.28, cy + h * 0.08, w * 0.21)]
    m = Image.new("L", img.size, 0)
    d = ImageDraw.Draw(m)
    for (x, y, r) in parts:
        d.ellipse([(x - r) * s, (y - r) * s, (x + r) * s, (y + r) * s], fill=255)
    d.rounded_rectangle([(cx - w * 0.46) * s, (cy + h * 0.02) * s, (cx + w * 0.49) * s, (cy + h * 0.36) * s],
                        radius=h * 0.17 * s, fill=255)
    # shade: lighter top-left, darker bottom
    grad = Image.new("RGBA", img.size)
    top = min(y - r for (x, y, r) in parts)
    bot = cy + h * 0.36
    gd = ImageDraw.Draw(grad)
    for y in range(int(top * s), int(bot * s) + 1):
        t = max(0.0, min(1.0, (y / s - top) / (bot - top)))
        t = t ** 2.2
        c = tuple(int(col[i] * (1 - t) + sh[i] * t) for i in range(3))
        gd.line([(0, y), (img.size[0], y)], fill=c + (255,))
    # a dark rim so the cloud separates from whatever is behind it
    rim = m.filter(ImageFilter.MaxFilter(int(2.2 * s) | 1))
    img.paste(Image.new("RGBA", img.size, (0, 0, 0, 255)), (0, 0), rim)
    img.paste(grad, (0, 0), m)

def drops(img, xs, y0, length, col=RAIN):
    s = img.size[0] / 100.0
    d = ImageDraw.Draw(img)
    for x in xs:
        d.line([(x * s, y0 * s), ((x - length * 0.35) * s, (y0 + length) * s)], fill=col, width=int(5.5 * s))
        for (px, py) in ((x, y0), (x - length * 0.35, y0 + length)):
            d.ellipse([(px - 2.75) * s, (py - 2.75) * s, (px + 2.75) * s, (py + 2.75) * s], fill=col)

def bolt(img, x, y):
    s = img.size[0] / 100.0
    pts = [(x + 4, y), (x - 9, y + 20), (x - 1, y + 20), (x - 6, y + 36), (x + 11, y + 13), (x + 2, y + 13), (x + 8, y)]
    ImageDraw.Draw(img).polygon([(px * s, py * s) for px, py in pts], fill=BOLT)

def flakes(img, pts):
    s = img.size[0] / 100.0
    d = ImageDraw.Draw(img)
    for (x, y) in pts:
        for k in range(3):
            a = k * math.pi / 3 + math.pi / 2
            d.line([((x - 6 * math.cos(a)) * s, (y - 6 * math.sin(a)) * s),
                    ((x + 6 * math.cos(a)) * s, (y + 6 * math.sin(a)) * s)], fill=SNOW, width=int(3.2 * s))

def bars(img, ys):
    s = img.size[0] / 100.0
    d = ImageDraw.Draw(img)
    for i, y in enumerate(ys):
        x0 = 16 + (i % 2) * 8
        d.rounded_rectangle([x0 * s, (y - 3.2) * s, (x0 + 64) * s, (y + 3.2) * s], radius=3.2 * s, fill=FOG)

def draw(name, n):
    img = canvas(n)
    if name == "sun":        sun(img, 50, 50, 21)
    elif name == "moon":     moon(img, 50, 50, 30)
    elif name == "part_day":
        sun(img, 62, 36, 15); cloud(img, 44, 60, 70)
    elif name == "part_night":
        moon(img, 60, 33, 22); cloud(img, 44, 62, 70)
    elif name == "cloud":    cloud(img, 50, 54, 86)
    elif name == "fog":
        cloud(img, 52, 40, 70); bars(img, (70, 80, 90))
    elif name == "rain":
        cloud(img, 50, 40, 80, CLOUD_DARK, CLOUD_DARK_SH); drops(img, (36, 54, 72), 66, 18)
    elif name == "storm":
        cloud(img, 50, 40, 80, CLOUD_DARK, CLOUD_DARK_SH); bolt(img, 52, 58); drops(img, (30, 76), 66, 14)
    elif name == "snow":
        cloud(img, 50, 40, 80); flakes(img, ((34, 72), (54, 84), (72, 70)))
    return img.resize((n, n), Image.LANCZOS)

NAMES = ["sun", "moon", "part_day", "part_night", "cloud", "fog", "rain", "storm", "snow"]   # = KwxIcon order
SIZES = {"big": 112, "small": 40}

def to_c(img, cname):
    px = img.load()
    n = img.size[0]
    out = bytearray()
    for y in range(n):
        for x in range(n):
            r, g, b, a = px[x, y]
            v = ((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3)
            out += bytes((v & 0xFF, v >> 8, a))
    lines = []
    for i in range(0, len(out), 24):
        lines.append("    " + ", ".join("0x%02x" % c for c in out[i:i + 24]) + ",")
    return (f"static const uint8_t {cname}_map[] = {{\n" + "\n".join(lines) + "\n};\n"
            f"const lv_img_dsc_t {cname} = {{\n"
            f"    .header = {{ .cf = LV_IMG_CF_TRUE_COLOR_ALPHA, .always_zero = 0, .reserved = 0, .w = {n}, .h = {n} }},\n"
            f"    .data_size = {len(out)},\n    .data = {cname}_map,\n}};\n\n")

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--preview")
    a = ap.parse_args()
    src = ["// Generated by tools/weather/make_icons.py - do not edit by hand.\n",
           "// Kevin's Orb: Weather screen icons, LVGL 8 true colour + alpha (RGB565, alpha byte).\n",
           '#include <lvgl.h>\n\n']
    sheet = Image.new("RGBA", (len(NAMES) * 120, 170), (0, 0, 0, 255))
    for i, nm in enumerate(NAMES):
        for size, n in SIZES.items():
            img = draw(nm, n)
            src.append(to_c(img, f"kwx_icon_{nm}_{size}"))
            if a.preview:
                sheet.alpha_composite(img, (i * 120 + (4 if size == "big" else 40), 4 if size == "big" else 124))
    src.append("const lv_img_dsc_t *const kwx_icons_big[] = { " + ", ".join(f"&kwx_icon_{n}_big" for n in NAMES) + " };\n")
    src.append("const lv_img_dsc_t *const kwx_icons_small[] = { " + ", ".join(f"&kwx_icon_{n}_small" for n in NAMES) + " };\n")
    with open(os.path.join(ROOT, "src", "kwx_icons.c"), "w") as f:
        f.writelines(src)
    if a.preview:
        sheet.convert("RGB").save(a.preview)
    print("wrote src/kwx_icons.c")

if __name__ == "__main__":
    main()

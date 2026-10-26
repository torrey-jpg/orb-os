#!/usr/bin/env python3
"""Kevin's Orb - the Bateman Dental steampunk clock face.

Draws the art for an Orb theme and writes the theme folder the firmware reads from the SD
card (the same layout Orb Studio installs):

  OUTDIR/<slug>/clock_plate.png         466 x 466 dial: aged brass, a copper riveted bezel,
                                        an etched gear, Roman numerals with molars at 12,
                                        3, 6 and 9, and a riveted name plate
  OUTDIR/<slug>/clock_hand_hour.png     blackened steel, a spade tip
  OUTDIR/<slug>/clock_hand_minute.png   blackened steel, a dental mirror at the tip
  OUTDIR/<slug>/clock_hand_second.png   copper needle with an explorer's hook, gear
                                        counterweight and the brass centre cap
  OUTDIR/<slug>/clock_shadow_*.png      soft shadows for the hour and minute hands
  OUTDIR/<slug>/clock_style.json        where each hand pivots
  OUTDIR/<slug>/theme.json, _installed  the theme's name, asset list and "ready" marker

Every hand is drawn pointing at 12 with its pivot recorded in clock_style.json; the firmware
turns it. Original artwork, drawn in code.

Usage: python3 make_steampunk.py OUTDIR [--name "BATEMAN DENTAL"] [--place "NACOGDOCHES, TX"]
Needs numpy and Pillow.
"""
import argparse, json, math, os
import numpy as np
from PIL import Image, ImageDraw, ImageFilter, ImageFont

SS = 3                      # supersampling
W = 466
C = W / 2
SLUG = "bateman-steampunk"

SERIF = "/usr/share/fonts/truetype/liberation/LiberationSerif-Bold.ttf"
PLATE_FONT = "/usr/share/fonts/truetype/liberation/LiberationSerif-Bold.ttf"
FALLBACK = "/usr/share/fonts/truetype/dejavu/DejaVuSerif-Bold.ttf"

def font(path, size):
    for p in (path, FALLBACK):
        try:
            return ImageFont.truetype(p, int(size))
        except OSError:
            pass
    return ImageFont.load_default()

def hexc(h, a=255):
    h = h.lstrip("#")
    return (int(h[0:2], 16), int(h[2:4], 16), int(h[4:6], 16), a)

# ------------------------------------------------------------------ shapes
def molar_poly(cx, cy, w, h, ang=0.0):
    """A molar silhouette, crown toward -y (outward once rotated), two roots toward +y."""
    pts = []
    # crown top: three cusps
    for i in range(31):
        t = i / 30
        x = -w / 2 + w * t
        y = -h * 0.50 + h * 0.07 * (1 - math.cos(t * 2 * math.pi * 1.5)) * 0.9
        pts.append((x, y))
    # right side down to the neck
    pts += [(w * 0.50, -h * 0.20), (w * 0.44, h * 0.02), (w * 0.36, h * 0.10)]
    # right root
    pts += [(w * 0.40, h * 0.30), (w * 0.30, h * 0.50), (w * 0.18, h * 0.50), (w * 0.12, h * 0.22)]
    # furcation
    pts += [(0, h * 0.14)]
    # left root
    pts += [(-w * 0.12, h * 0.22), (-w * 0.18, h * 0.50), (-w * 0.30, h * 0.50), (-w * 0.40, h * 0.30)]
    pts += [(-w * 0.36, h * 0.10), (-w * 0.44, h * 0.02), (-w * 0.50, -h * 0.20)]
    ca, sa = math.cos(ang), math.sin(ang)
    return [(cx + x * ca - y * sa, cy + x * sa + y * ca) for x, y in pts]

def gear_poly(cx, cy, r_out, r_in, teeth, phase=0.0):
    pts = []
    for i in range(teeth):
        a0 = phase + 2 * math.pi * i / teeth
        step = 2 * math.pi / teeth
        for f, r in ((0.0, r_in), (0.12, r_out), (0.45, r_out), (0.57, r_in)):
            a = a0 + f * step
            pts.append((cx + r * math.cos(a), cy + r * math.sin(a)))
    return pts

def rivet(d, x, y, r):
    d.ellipse((x - r, y - r, x + r, y + r), fill=hexc("#3A2410"))
    d.ellipse((x - r * 0.85, y - r * 0.85, x + r * 0.85, y + r * 0.85), fill=hexc("#B07A45"))
    d.ellipse((x - r * 0.55, y - r * 0.65, x + r * 0.15, y + r * 0.05), fill=hexc("#F0CB90"))

def engraved_text(img, xy, text, f, dark="#2A1B0B", light="#EED9A8", anchor="mm"):
    d = ImageDraw.Draw(img)
    x, y = xy
    d.text((x + SS * 0.8, y + SS * 0.8), text, font=f, fill=hexc(light, 150), anchor=anchor)
    d.text((x, y), text, font=f, fill=hexc(dark), anchor=anchor)

# ------------------------------------------------------------------ dial
def make_plate(name, place):
    N = W * SS
    yy, xx = np.mgrid[0:N, 0:N].astype(np.float32) / SS
    r = np.hypot(xx - C, yy - C)
    th = np.arctan2(yy - C, xx - C)
    rng = np.random.default_rng(3)

    # Aged brass: warm centre, darker toward the rim, with a machined circular grain.
    t = np.clip(r / 216.0, 0, 1)
    c0, c1 = np.array([204, 168, 102]), np.array([118, 86, 44])
    base = c0[None, None, :] * (1 - t[..., None] ** 1.6) + c1[None, None, :] * (t[..., None] ** 1.6)
    grain = 0.035 * np.sin(r * 2.7 + 0.6 * np.sin(th * 7)) + 0.02 * rng.standard_normal((N, N))
    light = 1 + grain + 0.10 * np.cos(th + 2.3) * t       # a soft light from the upper left
    img = np.clip(base * light[..., None], 0, 255)
    # Patina: faint green-brown blotches.
    blot = np.array(Image.fromarray((rng.random((60, 60)) * 255).astype(np.uint8)).resize((N, N), Image.BICUBIC), dtype=np.float32) / 255
    img = img * (1 - 0.10 * blot[..., None]) + np.array([70, 90, 60]) * 0.10 * blot[..., None]
    im = Image.fromarray(img.astype(np.uint8), "RGB").convert("RGBA")

    # Etched gear in the middle of the dial.
    layer = Image.new("RGBA", (N, N), (0, 0, 0, 0))
    d = ImageDraw.Draw(layer)
    g = [(x * SS, y * SS) for x, y in gear_poly(C, C, 128, 114, 18, 0.1)]
    d.polygon(g, fill=hexc("#5A4020", 120))
    d.ellipse([(C - 98) * SS, (C - 98) * SS, (C + 98) * SS, (C + 98) * SS], fill=(0, 0, 0, 0))
    lay2 = Image.new("RGBA", (N, N), (0, 0, 0, 0))
    d2 = ImageDraw.Draw(lay2)
    d2.ellipse([(C - 98) * SS, (C - 98) * SS, (C + 98) * SS, (C + 98) * SS], outline=hexc("#5A4020", 120), width=10 * SS)
    for k in range(6):
        a = k * math.pi / 3 + 0.3
        d2.line([(C + 26 * math.cos(a)) * SS, (C + 26 * math.sin(a)) * SS,
                 (C + 94 * math.cos(a)) * SS, (C + 94 * math.sin(a)) * SS], fill=hexc("#5A4020", 120), width=9 * SS)
    d2.ellipse([(C - 30) * SS, (C - 30) * SS, (C + 30) * SS, (C + 30) * SS], fill=hexc("#5A4020", 120))
    layer = Image.alpha_composite(layer, lay2)
    hi = layer.copy().split()[3].point(lambda v: int(v * 0.55))
    hi_img = Image.new("RGBA", (N, N), hexc("#F3D9A0"))
    hi_img.putalpha(hi)
    im.alpha_composite(hi_img, (SS, SS))          # engraved: light edge below-right
    im.alpha_composite(layer)

    d = ImageDraw.Draw(im)
    def ring(rad, width, col):
        d.ellipse([(C - rad) * SS, (C - rad) * SS, (C + rad) * SS, (C + rad) * SS], outline=hexc(col), width=int(width * SS))
    # Chapter ring lines (engraved: dark line with a light line just inside).
    for rad in (150, 201):
        ring(rad + 0.8, 1.2, "#F0D9A6")
        ring(rad, 1.6, "#3A2611")

    # Minute track.
    for m in range(60):
        a = m * math.pi / 30 - math.pi / 2
        r0 = 203 if m % 5 else 199
        wdt = 1.4 if m % 5 else 3.0
        d.line([(C + r0 * math.cos(a)) * SS, (C + r0 * math.sin(a)) * SS,
                (C + 213 * math.cos(a)) * SS, (C + 213 * math.sin(a)) * SS], fill=hexc("#2E1E0C"), width=int(wdt * SS))

    # Roman numerals, with molars at 12, 3, 6 and 9.
    romans = ["XII", "I", "II", "III", "IV", "V", "VI", "VII", "VIII", "IX", "X", "XI"]
    f = font(SERIF, 24 * SS)
    for h in range(12):
        a = h * math.pi / 6 - math.pi / 2
        x, y = C + 176 * math.cos(a), C + 176 * math.sin(a)
        if h % 3 == 0:
            poly = [(px * SS, py * SS) for px, py in molar_poly(x, y, 30, 36, a + math.pi / 2)]
            sh = [(px + 1.5 * SS, py + 1.5 * SS) for px, py in poly]
            d.polygon(sh, fill=hexc("#2A1B0B", 160))
            d.polygon(poly, fill=hexc("#F2EBDD"), outline=hexc("#3A2611"))
            # a little shine on the crown
            cx_, cy_ = x + 6 * math.cos(a), y + 6 * math.sin(a)
            d.ellipse([(cx_ - 5) * SS, (cy_ - 3) * SS, (cx_ + 2) * SS, (cy_ + 2) * SS], fill=hexc("#FFFFFF", 170))
        else:
            engraved_text(im, (x * SS, y * SS), romans[h], f)

    # Name plate, riveted.
    pw, ph, py0 = 186, 46, 300
    x0, x1 = C - pw / 2, C + pw / 2
    d.rounded_rectangle([(x0 + 2) * SS, (py0 + 3) * SS, (x1 + 2) * SS, (py0 + ph + 3) * SS], radius=7 * SS, fill=hexc("#2A1B0B", 150))
    d.rounded_rectangle([x0 * SS, py0 * SS, x1 * SS, (py0 + ph) * SS], radius=7 * SS, fill=hexc("#D8B676"), outline=hexc("#5A3A18"), width=2 * SS)
    d.rounded_rectangle([(x0 + 4) * SS, (py0 + 4) * SS, (x1 - 4) * SS, (py0 + ph - 4) * SS], radius=5 * SS, outline=hexc("#A9874E"), width=1 * SS)
    for rx in (x0 + 10, x1 - 10):
        for ry in (py0 + 10, py0 + ph - 10):
            rivet(d, rx * SS, ry * SS, 3.2 * SS)
    engraved_text(im, (C * SS, (py0 + 18) * SS), name, font(PLATE_FONT, 17 * SS), light="#F6E4B8")
    engraved_text(im, (C * SS, (py0 + 35) * SS), place, font(PLATE_FONT, 9.5 * SS), light="#F6E4B8")

    # Copper bezel with rivets.
    d = ImageDraw.Draw(im)
    bez = np.zeros((N, N, 4), np.uint8)
    band = (r >= 216) & (r <= 234)
    tt = np.clip((r - 216) / 18, 0, 1)
    shade = 0.75 + 0.45 * np.sin(tt * math.pi) * (0.75 + 0.25 * np.cos(th + 2.3))
    cu = np.array([178, 104, 58])
    bez[..., :3] = np.clip(cu[None, None, :] * shade[..., None], 0, 255).astype(np.uint8)
    bez[..., 3] = band.astype(np.uint8) * 255
    im.alpha_composite(Image.fromarray(bez, "RGBA"))
    d = ImageDraw.Draw(im)
    ring(216, 2, "#3A1E0C")
    for k in range(24):
        a = k * math.pi / 12 + math.pi / 24
        rivet(d, (C + 225 * math.cos(a)) * SS, (C + 225 * math.sin(a)) * SS, 3.4 * SS)

    # Soft vignette and the round mask's black outside.
    out = im.convert("RGB").resize((W, W), Image.LANCZOS)
    a = np.array(out, np.float32)
    yy, xx = np.mgrid[0:W, 0:W]
    rr = np.hypot(xx - C + 0.5, yy - C + 0.5)
    a *= (1 - 0.18 * np.clip((rr - 150) / 85, 0, 1) ** 2)[..., None]
    a[rr > 233.5] = 0
    return Image.fromarray(a.astype(np.uint8), "RGB")

# ------------------------------------------------------------------ hands
def blackened(d, poly):
    d.polygon(poly, fill=hexc("#1C1814"), outline=hexc("#6E6050"))

def make_hour():
    w, h, px, py = 64, 172, 32, 142
    im = Image.new("RGBA", (w * SS, h * SS), (0, 0, 0, 0))
    d = ImageDraw.Draw(im)
    S = lambda pts: [(x * SS, y * SS) for x, y in pts]
    # tail and stem
    blackened(d, S([(px - 5, py + 26), (px + 5, py + 26), (px + 6, py), (px + 4, 66), (px - 4, 66), (px - 6, py)]))
    d.ellipse(S([(px - 9, py + 20), (px + 9, py + 36)]), fill=hexc("#1C1814"), outline=hexc("#6E6050"))
    # Breguet-style eye
    d.ellipse(S([(px - 13, 70), (px + 13, 96)]), outline=hexc("#1C1814"), width=5 * SS)
    d.ellipse(S([(px - 13, 70), (px + 13, 96)]), outline=hexc("#6E6050"), width=1 * SS)
    # pivot boss
    d.ellipse(S([(px - 14, py - 14), (px + 14, py + 14)]), fill=hexc("#1C1814"), outline=hexc("#6E6050"))
    # a classic spade tip: pointed, with rounded lobes and a small open window
    right = [(4, 12), (9, 20), (13, 28), (14.5, 34), (13.5, 40), (10, 45), (6, 48), (3, 52), (2.5, 66)]
    spade = [(px, 3)] + [(px + dx, y) for dx, y in right] + [(px - dx, y) for dx, y in reversed(right)]
    d.polygon(S(spade), fill=hexc("#1C1814"), outline=hexc("#6E6050"))
    d.ellipse(S([(px - 4, 29), (px + 4, 40)]), fill=(0, 0, 0, 0), outline=hexc("#6E6050"), width=SS)
    return im.resize((w, h), Image.LANCZOS), px, py

def make_minute():
    w, h, px, py = 48, 222, 24, 196
    im = Image.new("RGBA", (w * SS, h * SS), (0, 0, 0, 0))
    d = ImageDraw.Draw(im)
    S = lambda pts: [(x * SS, y * SS) for x, y in pts]
    blackened(d, S([(px - 4, py + 28), (px + 4, py + 28), (px + 5, py), (px + 2.5, 40), (px - 2.5, 40), (px - 5, py)]))
    d.ellipse(S([(px - 8, py + 22), (px + 8, py + 36)]), fill=hexc("#1C1814"), outline=hexc("#6E6050"))
    d.ellipse(S([(px - 9, 104), (px + 9, 124)]), outline=hexc("#1C1814"), width=4 * SS)
    d.ellipse(S([(px - 12, py - 12), (px + 12, py + 12)]), fill=hexc("#1C1814"), outline=hexc("#6E6050"))
    # dental mirror: a short neck and a round mirror in a dark rim
    d.line(S([(px, 42), (px, 30)]), fill=hexc("#1C1814"), width=4 * SS)
    d.ellipse(S([(px - 16, 2), (px + 16, 34)]), fill=hexc("#1C1814"))
    mir = Image.new("RGBA", (32 * SS, 32 * SS), (0, 0, 0, 0))
    yy, xx = np.mgrid[0:32 * SS, 0:32 * SS] / SS
    rr = np.hypot(xx - 16, yy - 16)
    v = np.clip(200 + 50 * ((16 - xx) + (16 - yy)) / 22, 120, 255)
    arr = np.zeros((32 * SS, 32 * SS, 4), np.uint8)
    arr[..., 0] = (v * 0.86).astype(np.uint8); arr[..., 1] = (v * 0.93).astype(np.uint8); arr[..., 2] = v.astype(np.uint8)
    arr[..., 3] = (rr < 12.5).astype(np.uint8) * 255
    mir = Image.fromarray(arr, "RGBA")
    im.alpha_composite(mir, (int((px - 16) * SS), int(2 * SS)))
    d = ImageDraw.Draw(im)
    d.ellipse(S([(px - 9, 8), (px - 3, 13)]), fill=hexc("#FFFFFF", 230))
    d.ellipse(S([(px - 13, 5), (px + 13, 31)]), outline=hexc("#6E6050"), width=1 * SS)
    return im.resize((w, h), Image.LANCZOS), px, py

def make_second():
    w, h, px, py = 34, 252, 17, 206
    im = Image.new("RGBA", (w * SS, h * SS), (0, 0, 0, 0))
    d = ImageDraw.Draw(im)
    S = lambda pts: [(x * SS, y * SS) for x, y in pts]
    red = hexc("#B4462C")
    d.polygon(S([(px - 1.6, py), (px + 1.6, py), (px + 0.8, 20), (px - 0.8, 20)]), fill=red)
    # explorer's hook at the tip
    d.arc(S([(px - 1, 8), (px + 11, 24)]), start=180, end=330, fill=red, width=int(1.6 * SS))
    # counterweight: a little gear
    gp = S(gear_poly(px, py + 30, 10, 8, 10))
    d.line(S([(px, py), (px, py + 24)]), fill=red, width=3 * SS)
    d.polygon(gp, fill=red, outline=hexc("#5A1E10"))
    d.ellipse(S([(px - 3.5, py + 26.5), (px + 3.5, py + 33.5)]), fill=hexc("#5A1E10"))
    # brass centre cap, gear-edged, over every hand
    cap = S(gear_poly(px, py, 13, 11, 12))
    d.polygon(cap, fill=hexc("#C99A52"), outline=hexc("#4A2E12"))
    d.ellipse(S([(px - 7, py - 7), (px + 7, py + 7)]), fill=hexc("#E2BC74"), outline=hexc("#4A2E12"))
    d.ellipse(S([(px - 2.5, py - 2.5), (px + 2.5, py + 2.5)]), fill=hexc("#2A1B0B"))
    return im.resize((w, h), Image.LANCZOS), px, py

def shadow_of(hand):
    a = hand.split()[3].filter(ImageFilter.GaussianBlur(3))
    a = a.point(lambda v: int(v * 0.55))
    sh = Image.new("RGBA", hand.size, (0, 0, 0, 255))
    sh.putalpha(a)
    return sh

# ------------------------------------------------------------------ theme folder
def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("outdir")
    ap.add_argument("--name", default="BATEMAN DENTAL")
    ap.add_argument("--place", default="NACOGDOCHES, TX")
    a = ap.parse_args()
    d = os.path.join(a.outdir, SLUG)
    os.makedirs(d, exist_ok=True)

    # RGBA even though the dial is opaque: the firmware's PNG loader reads RGBA pixels only
    # (an RGB file decodes to black), and Orb Studio always exports RGBA.
    make_plate(a.name, a.place).convert("RGBA").save(os.path.join(d, "clock_plate.png"), optimize=True)
    hands = {}
    for key, fn in (("hour", make_hour), ("minute", make_minute), ("second", make_second)):
        im, px, py = fn()
        im.save(os.path.join(d, f"clock_hand_{key}.png"), optimize=True)
        hands[key] = {"show": True, "pivotX": px, "pivotY": py, "centerX": 233, "centerY": 233}
        if key != "second":
            shadow_of(im).save(os.path.join(d, f"clock_shadow_{key}.png"), optimize=True)

    hands["static1"] = {"show": False}
    hands["static2"] = {"show": False}
    hands["order"] = [0, 1, 2]                        # hour, minute, then second (with the cap) on top
    hands["shadow"] = {"on": True, "dx": 3, "dy": 5}
    style = {"bg": 0, "hands": hands,
             "text1": {"show": False}, "text2": {"show": False}}
    with open(os.path.join(d, "clock_style.json"), "w") as f:
        json.dump(style, f, indent=2)

    assets = sorted(n for n in os.listdir(d) if n.endswith(".png"))
    theme = {"name": "Bateman Steampunk", "author": "Kevin's Orb", "assets": assets}
    with open(os.path.join(d, "theme.json"), "w") as f:
        json.dump(theme, f, indent=2)
    open(os.path.join(d, "_installed"), "w").close()
    print(f"wrote {d}: {', '.join(assets)}")

if __name__ == "__main__":
    main()

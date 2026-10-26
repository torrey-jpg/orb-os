#!/usr/bin/env python3
"""Kevin's Orb - prepare the photo for the startup greeting.

Two shapes, both written as the PNG the greeting loads from the SD card,
/greeting/photo.png, with a soft transparent edge:

  Portrait (default): a square crop scaled to 150 x 150 and cut to a circle.
      python3 make_photo.py PICTURE OUT.png [--zoom 1.0] [--x 0.5] [--y 0.4]
        --zoom  >1 crops tighter (closer on the face)
        --x/--y the centre of the crop, as a fraction of the picture's width/height

  Group: a wide rounded rectangle, 290 x 131, from a box you choose in the picture.
      python3 make_photo.py PICTURE OUT.png --box X0,Y0,X1,Y1
        the box in the picture's own pixels; it is widened or heightened about its
        centre to the 290:131 shape so nobody is stretched.
Needs Pillow.
"""
import argparse
from PIL import Image, ImageDraw, ImageOps

SIZE = 150
WIDE = (290, 131)    # fits across the top of the round dial inside the greeting ring
RADIUS = 18

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("picture")
    ap.add_argument("out")
    ap.add_argument("--zoom", type=float, default=1.0)
    ap.add_argument("--x", type=float, default=0.5)
    ap.add_argument("--y", type=float, default=0.4)
    ap.add_argument("--box", help="X0,Y0,X1,Y1 for a group photo")
    a = ap.parse_args()

    im = ImageOps.exif_transpose(Image.open(a.picture)).convert("RGB")
    w, h = im.size
    if a.box:
        x0, y0, x1, y1 = (float(v) for v in a.box.split(","))
        cx, cy = (x0 + x1) / 2, (y0 + y1) / 2
        bw, bh = x1 - x0, y1 - y0
        want = WIDE[0] / WIDE[1]
        if bw / bh < want: bw = bh * want
        else:              bh = bw / want
        box = (int(cx - bw / 2), int(cy - bh / 2), int(cx + bw / 2), int(cy + bh / 2))
        im = im.crop(box).resize(WIDE, Image.LANCZOS)
        big = Image.new("L", (WIDE[0] * 4, WIDE[1] * 4), 0)
        ImageDraw.Draw(big).rounded_rectangle((0, 0, WIDE[0] * 4 - 1, WIDE[1] * 4 - 1), radius=RADIUS * 4, fill=255)
        out = im.convert("RGBA")
        out.putalpha(big.resize(WIDE, Image.LANCZOS))
        out.save(a.out, "PNG", optimize=True)
        print(f"wrote {a.out} ({WIDE[0]}x{WIDE[1]}) from box {box}")
        return
    side = min(w, h) / max(a.zoom, 1e-3)
    cx, cy = a.x * w, a.y * h
    left = min(max(cx - side / 2, 0), w - side)
    top = min(max(cy - side / 2, 0), h - side)
    im = im.crop((int(left), int(top), int(left + side), int(top + side)))
    im = im.resize((SIZE, SIZE), Image.LANCZOS)

    # Circle mask drawn 4x and scaled down, for a smooth edge on a round screen.
    big = Image.new("L", (SIZE * 4, SIZE * 4), 0)
    ImageDraw.Draw(big).ellipse((0, 0, SIZE * 4 - 1, SIZE * 4 - 1), fill=255)
    mask = big.resize((SIZE, SIZE), Image.LANCZOS)

    out = im.convert("RGBA")
    out.putalpha(mask)
    out.save(a.out, "PNG", optimize=True)
    print(f"wrote {a.out} ({SIZE}x{SIZE})")

if __name__ == "__main__":
    main()

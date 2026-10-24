#!/usr/bin/env python3
"""Kevin's Orb - prepare the photo for the startup greeting.

Takes any picture (JPG, PNG, HEIC if Pillow can read it), crops a square, scales it to
150 x 150 and cuts it to a circle with a soft transparent edge, then writes the PNG the
greeting loads from the SD card: /greeting/photo.png.

Usage:
    python3 make_photo.py PICTURE OUT.png [--zoom 1.0] [--x 0.5] [--y 0.4]

  --zoom  >1 crops tighter (closer on the face)
  --x/--y the centre of the crop, as a fraction of the picture's width/height
          (0.5, 0.4 = middle, a little above centre, where faces usually are)
Needs Pillow.
"""
import argparse
from PIL import Image, ImageDraw, ImageOps

SIZE = 150

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("picture")
    ap.add_argument("out")
    ap.add_argument("--zoom", type=float, default=1.0)
    ap.add_argument("--x", type=float, default=0.5)
    ap.add_argument("--y", type=float, default=0.4)
    a = ap.parse_args()

    im = ImageOps.exif_transpose(Image.open(a.picture)).convert("RGB")
    w, h = im.size
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

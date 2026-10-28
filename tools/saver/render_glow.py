#!/usr/bin/env python3
"""Kevin's Orb - render the "glow" screensaver's background picture.

An original scene: a lower first molar standing on a dark forest floor like a glowing
mushroom. The crown is deep indigo enamel covered in luminous spots (blue, cyan, violet,
a few gold and pink), a warm glowing band where crown meets root, warm glowing roots, and
soft out-of-focus lights behind. The colours keep away from red-against-green, because
Dr. Hollar is red/green colour blind.

The tooth shape is the Tooth Explorer's own (tools/teeth/render_teeth.py), ray-marched here
with different lighting. Output is 466 x 466, already rounded to the screen's 16-bit
colour (RGB565) with dithering, so smooth glows do not band on the Orb.

It also writes the screen positions of the brightest spots to a C header; the firmware
pulses a glow over each of them.

Usage:  python3 tools/saver/render_glow.py OUT.png [--header src/saver_glow_spots.h] [--seed 7]
Needs numpy, scipy and Pillow.
"""
import argparse, os, sys
import numpy as np
from PIL import Image
from scipy import ndimage

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "teeth"))
from render_teeth import TYPES, tooth_sdf, rot_x, rot_y, length, V   # noqa: E402

SIZE = 466
SS = 2                      # supersampling
N = SIZE * SS
TOOTH = TYPES[10]           # Lower First Molar: crown up, two roots down - a mushroom shape
SCALE_MM = 33.0             # mm across the frame
CENTER_Y_MM = -1.2          # where the frame centre sits on the tooth's long axis
YAW, PITCH = 28.0, 16.0     # turn a little, and look down onto the crown a little

def smooth(e0, e1, x):
    t = np.clip((x - e0) / (e1 - e0), 0, 1)
    return t * t * (3 - 2 * t)

def hash3(c, seed):
    """Deterministic pseudo-random numbers in [0,1) for integer cell coords (...,3)."""
    c = c.astype(np.int64)
    h = (c[..., 0] * 73856093) ^ (c[..., 1] * 19349663) ^ (c[..., 2] * 83492791) ^ (seed * 2654435761)
    h = (h ^ (h >> 13)) * 1274126177
    h = h ^ (h >> 16)
    return (h & 0xFFFFFF) / float(0x1000000)

def worley(p, cell, seed):
    """Distance to the nearest random feature point (in cell units) and that point's id hash."""
    q = p / cell
    base = np.floor(q)
    best = np.full(q.shape[:-1], 9.0)
    bid = np.zeros(q.shape[:-1])
    bpt = np.zeros(q.shape)
    for dx in (-1, 0, 1):
        for dy in (-1, 0, 1):
            for dz in (-1, 0, 1):
                c = base + np.array([dx, dy, dz])
                jit = np.stack([hash3(c, seed + k) for k in range(3)], axis=-1)
                fp = c + 0.15 + 0.7 * jit
                d = length(q - fp)
                m = d < best
                best = np.where(m, d, best)
                bid = np.where(m, hash3(c, seed + 9), bid)
                bpt = np.where(m[..., None], fp * cell, bpt)
    return best, bid, bpt

# ---------------------------------------------------------------- the tooth
def render_tooth(seed):
    ch, rl, md, bl = TOOTH[2], TOOTH[5], TOOTH[3], TOOTH[4]
    u = (np.arange(N) + 0.5) / N - 0.5
    px, py = np.meshgrid(u * SCALE_MM, -u * SCALE_MM)
    R = rot_x(np.radians(PITCH)) @ rot_y(np.radians(YAW))
    M = R.T
    o = np.stack([px, py, np.full_like(px, 40.0)], axis=-1).reshape(-1, 3)
    O = o @ M.T + np.array([0.0, CENTER_Y_MM, 0.0])
    D = M @ np.array([0.0, 0.0, -1.0])
    mid = (ch - rl) / 2.0
    c = np.array([0.0, mid, 0.0])
    rad = np.sqrt(((ch + rl) / 2 + 1.5) ** 2 + (max(md, bl) / 2 + 2.0) ** 2)
    oc = O - c
    b = oc @ D
    disc = b * b - (np.sum(oc * oc, axis=1) - rad * rad)
    cand = np.nonzero(disc > 0)[0]
    tt = np.zeros(O.shape[0]); tmax = np.zeros(O.shape[0])
    tt[cand] = -b[cand] - np.sqrt(disc[cand]); tmax[cand] = -b[cand] + np.sqrt(disc[cand])
    hit = np.zeros(O.shape[0], bool); alive = np.zeros(O.shape[0], bool); alive[cand] = True
    for _ in range(160):
        idx = np.nonzero(alive)[0]
        if idx.size == 0:
            break
        pos = O[idx] + tt[idx, None] * D
        dist = tooth_sdf(pos, TOOTH)
        tt[idx] += dist * 0.6
        done = dist < 0.008
        hit[idx[done]] = True
        alive[idx[done | (tt[idx] > tmax[idx])]] = False

    col = np.zeros((O.shape[0], 3))
    emis = np.zeros((O.shape[0], 3))
    hi = np.nonzero(hit)[0]
    P = O[hi] + tt[hi, None] * D
    e = 0.02
    nrm = np.stack([tooth_sdf(P + V(e, 0, 0), TOOTH) - tooth_sdf(P - V(e, 0, 0), TOOTH),
                    tooth_sdf(P + V(0, e, 0), TOOTH) - tooth_sdf(P - V(0, e, 0), TOOTH),
                    tooth_sdf(P + V(0, 0, e), TOOTH) - tooth_sdf(P - V(0, 0, e), TOOTH)], axis=-1)
    nrm /= np.maximum(length(nrm)[:, None], 1e-9)
    Nc = nrm @ M                               # camera space
    y = P[:, 1]
    L1 = V(-0.55, 0.65, 0.5); L1 /= np.linalg.norm(L1)
    Vw = V(0, 0, 1)
    dif = np.clip(Nc @ L1, 0, 1)
    fres = (1 - np.clip(Nc @ Vw, 0, 1)) ** 2.5
    Hh = L1 + Vw; Hh /= np.linalg.norm(Hh)
    spec = np.clip(Nc @ Hh, 0, 1) ** 30

    crown = smooth(0.2, 1.4, y)               # 0 on the root, 1 on the crown
    # --- crown: indigo enamel, lit softly, violet rim light
    indigo = V(0.10, 0.08, 0.32)
    c_crown = indigo * (0.35 + 0.9 * dif[:, None]) + spec[:, None] * V(0.5, 0.6, 1.0) * 0.6 \
        + fres[:, None] * V(0.45, 0.35, 1.0) * 0.9
    # spots: worley cells on the surface
    d, sid, spt = worley(P, 1.55, seed)
    on = smooth(0.48, 0.40, d) * smooth(0.9, 1.6, y)          # inside a spot, crown only
    rim = smooth(0.30, 0.44, d) * on                           # bright ring at the edge
    grain = 0.75 + 0.25 * np.sin(P[:, 0] * 9.0 + P[:, 2] * 7.0) * np.sin(P[:, 1] * 11.0)
    palette = np.array([[0.30, 0.85, 1.00],   # cyan
                        [0.35, 0.60, 1.00],   # blue
                        [0.40, 0.90, 1.00],   # cyan
                        [0.65, 0.48, 1.00],   # violet
                        [0.30, 0.70, 1.00],   # blue
                        [1.00, 0.45, 0.85],   # pink
                        [1.00, 0.78, 0.35],   # gold
                        [0.45, 0.80, 1.00]])  # sky
    pc = palette[(sid * len(palette)).astype(int) % len(palette)]
    spot_e = pc * (on * (0.45 * grain) + rim * 1.25)[:, None]
    c_crown = c_crown * (1 - 0.6 * on[:, None]) + spot_e * 0.6
    # --- the glowing band where crown meets root (the mushroom's gills)
    band = np.exp(-((y - 0.25) / 0.75) ** 2)
    under = np.clip(-Nc[:, 1] * 0.6 + 0.6, 0, 1)
    band_e = V(1.0, 0.70, 0.32) * (band * under)[:, None] * 1.3
    # streaks in the band
    ang = np.arctan2(P[:, 2], P[:, 0])
    streak = 0.6 + 0.4 * np.sin(ang * 46.0 + np.sin(ang * 7) * 2)
    band_e *= streak[:, None]
    # --- roots: warm peach, fibrous, brighter near the top
    fib = 0.80 + 0.20 * np.sin(ang * 30.0 + P[:, 1] * 0.8) * np.sin(ang * 13.0 - P[:, 1] * 0.5)
    topglow = smooth(-13.0, 0.0, y)
    c_root = V(0.95, 0.55, 0.38) * (0.25 + 0.55 * dif + 0.45 * topglow)[:, None] * fib[:, None] \
        + fres[:, None] * V(1.0, 0.55, 0.75) * 0.5
    root_e = V(1.0, 0.58, 0.35) * (topglow ** 2 * 0.35)[:, None]
    c = c_root * (1 - crown[:, None]) + c_crown * crown[:, None]
    em = spot_e * crown[:, None] + band_e + root_e * (1 - crown[:, None])
    col[hi] = c + band_e * 0.8
    emis[hi] = em
    mask = hit.astype(float)
    # the spot centres, for the firmware's pulsing glows
    centers = spt[(on > 0.5)]
    cid = sid[(on > 0.5)]
    return col.reshape(N, N, 3), emis.reshape(N, N, 3), mask.reshape(N, N), (centers, cid, M)

def project(pts, M):
    """tooth space -> screen pixels (final resolution)."""
    q = (pts - np.array([0.0, CENTER_Y_MM, 0.0])) @ M      # camera space (inverse of M.T)
    x = (q[:, 0] / SCALE_MM + 0.5) * SIZE
    yy = (0.5 - q[:, 1] / SCALE_MM) * SIZE
    return x, yy, q[:, 2]

# ---------------------------------------------------------------- the world around it
def disc(img, cx, cy, r, color, alpha, soft):
    yy, xx = np.ogrid[:N, :N]
    d = np.sqrt((xx - cx) ** 2 + (yy - cy) ** 2)
    a = smooth(r, r - soft, d) * alpha
    # bokeh: slightly brighter rim
    a = a * (0.85 + 0.15 * smooth(r * 0.6, r * 0.95, d))
    img += a[..., None] * np.array(color)

def background(rng, tooth_mask):
    yy, xx = np.mgrid[:N, :N] / N
    r = np.sqrt((xx - 0.5) ** 2 + (yy - 0.45) ** 2)
    bg = np.zeros((N, N, 3))
    bg += (V(0.10, 0.06, 0.24) * (1 - smooth(0.0, 0.75, r))[..., None])
    bg += V(0.015, 0.01, 0.04)
    # a warm haze upper left, a violet haze right, like stage light
    bg += V(0.35, 0.16, 0.05) * np.exp(-(((xx - 0.22) / 0.16) ** 2 + ((yy - 0.18) / 0.12) ** 2))[..., None] * 0.6
    bg += V(0.30, 0.10, 0.40) * np.exp(-(((xx - 0.82) / 0.18) ** 2 + ((yy - 0.40) / 0.25) ** 2))[..., None] * 0.6
    bok = np.zeros((N, N, 3))
    lights = [  # x, y, radius (final px), colour, alpha
        (0.24, 0.17, 34, (1.0, 0.55, 0.20), 0.55), (0.14, 0.33, 22, (0.35, 0.55, 1.0), 0.45),
        (0.80, 0.30, 30, (0.85, 0.35, 0.75), 0.45), (0.86, 0.52, 16, (0.55, 0.40, 1.0), 0.5),
        (0.12, 0.55, 14, (1.0, 0.75, 0.30), 0.45), (0.30, 0.78, 10, (1.0, 0.65, 0.30), 0.55),
        (0.70, 0.12, 12, (0.40, 0.75, 1.0), 0.40), (0.62, 0.22, 7, (1.0, 0.80, 0.50), 0.5),
        (0.18, 0.72, 8, (0.95, 0.45, 0.85), 0.5), (0.90, 0.68, 9, (0.40, 0.85, 1.0), 0.45),
        (0.40, 0.10, 18, (0.55, 0.35, 0.90), 0.30), (0.08, 0.45, 9, (0.40, 0.85, 1.0), 0.35),
        (0.76, 0.80, 7, (1.0, 0.70, 0.35), 0.45), (0.52, 0.06, 9, (0.95, 0.45, 0.85), 0.35),
    ]
    for x, y, rad, colr, al in lights:
        disc(bok, x * N, y * N, rad * SS, colr, al, rad * SS * 0.35)
    bok = ndimage.gaussian_filter(bok, sigma=(2.2 * SS, 2.2 * SS, 0))
    bg += bok
    # ground: a textured dark floor from ~80% down, lit warm under the tooth and violet away
    horizon = 0.80
    gm = smooth(horizon - 0.02, horizon + 0.05, yy)
    tex = ndimage.gaussian_filter(rng.random((N, N)), 1.2 * SS) * 2.0
    tex2 = ndimage.gaussian_filter(rng.random((N, N)), 5 * SS) * 3.0
    pool = np.exp(-(((xx - 0.5) / 0.22) ** 2 + ((yy - 0.84) / 0.06) ** 2))
    floor = V(0.10, 0.05, 0.16) * (0.5 + 0.6 * tex[..., None] * tex2[..., None]) \
        + V(0.75, 0.40, 0.25) * (pool * (0.55 + 0.6 * tex))[..., None] * 0.55 \
        + V(0.30, 0.15, 0.45) * (0.3 * tex2)[..., None] * smooth(0.1, 0.5, np.abs(xx - 0.5))[..., None]
    bg = bg * (1 - gm[..., None]) + floor * gm[..., None]
    emis = bok * 0.6
    return bg, emis

def stalks(img, emis, rng):
    """Thin glowing stems with lit tips, bottom right (and two on the left)."""
    tips = []
    groups = [(0.80, 0.84, 9, 1.0), (0.22, 0.86, 5, 0.75)]
    cols = [(0.40, 0.85, 1.0), (0.95, 0.45, 0.85), (0.55, 0.45, 1.0), (0.40, 0.65, 1.0), (1.0, 0.78, 0.35)]
    for gx, gy, n, s in groups:
        for i in range(n):
            ang = (rng.random() - 0.5) * 0.9
            ln = (0.12 + rng.random() * 0.16) * s
            bend = (rng.random() - 0.5) * 0.06
            x0, y0 = gx * N + (rng.random() - 0.5) * 30 * SS, gy * N
            col = cols[rng.integers(len(cols))]
            pts = 140
            for k in range(pts):
                t = k / (pts - 1)
                x = x0 + np.sin(ang) * ln * N * t + bend * N * t * t
                y = y0 - np.cos(ang) * ln * N * t
                xi, yi = int(x), int(y)
                if 1 <= xi < N - 1 and 1 <= yi < N - 1:
                    a = 0.10 + 0.25 * t
                    img[yi, xi] += np.array(col) * a
                    img[yi, xi + 1] += np.array(col) * a * 0.5
            tips.append((x, y, col))
    tipl = np.zeros_like(img)
    for x, y, col in tips:
        disc(tipl, x, y, (3.0 + rng.random() * 2.5) * SS, col, 0.95, 1.5 * SS)
    img += tipl
    emis += tipl * 1.2

# ---------------------------------------------------------------- finish
def to_rgb565_dithered(img, rng):
    """Round to the screen's 5-6-5 bits with noise dithering; values land exactly on what
    the firmware's (v & 0xF8) / (v & 0xFC) conversion keeps."""
    out = np.empty(img.shape, np.uint8)
    for ch, bits in enumerate((5, 6, 5)):
        step = 256 // (1 << bits)
        v = img[..., ch] * 255.0 + (rng.random(img.shape[:2]) - 0.5) * step
        q = np.clip(np.floor(v / step + 0.5) * step, 0, 255 - (step - 1))
        out[..., ch] = q.astype(np.uint8)
    return out

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("out")
    ap.add_argument("--header")
    ap.add_argument("--seed", type=int, default=7)
    a = ap.parse_args()
    rng = np.random.default_rng(a.seed)
    col, temis, mask, (centers, cid, M) = render_tooth(a.seed)
    bg, bemis = background(rng, mask)
    stalks(bg, bemis, rng)
    # under-glow on the floor right beneath the tooth
    img = bg * (1 - mask[..., None]) + col * mask[..., None]
    em = bemis * (1 - mask[..., None]) + temis * mask[..., None]
    # bloom: two radii
    bloom = ndimage.gaussian_filter(em, sigma=(4 * SS, 4 * SS, 0)) * 0.9 \
        + ndimage.gaussian_filter(em, sigma=(14 * SS, 14 * SS, 0)) * 0.9
    img = img + bloom
    # tone map softly so hot spots stay coloured
    img = 1 - np.exp(-img * 1.25)
    img = img.reshape(SIZE, SS, SIZE, SS, 3).mean(axis=(1, 3))
    # round screen: gentle vignette toward the rim
    yy, xx = np.mgrid[:SIZE, :SIZE]
    r = np.sqrt((xx - SIZE / 2 + 0.5) ** 2 + (yy - SIZE / 2 + 0.5) ** 2) / (SIZE / 2)
    img *= (1 - 0.55 * smooth(0.70, 1.0, r))[..., None]
    img = np.clip(img, 0, 1)
    Image.fromarray(to_rgb565_dithered(img, rng), "RGB").save(a.out, optimize=True)
    print("wrote", a.out, os.path.getsize(a.out), "bytes")

    if a.header:
        # unique spot centres that face the camera, brightest-looking first
        uniq = {}
        for p, i in zip(centers, cid):
            uniq.setdefault(round(float(i), 6), p)
        pts = np.array(list(uniq.values()))
        x, y, z = project(pts, M)
        order = np.argsort(-z)                     # nearest to the camera first
        chosen = []
        for k in order:
            if not (40 < x[k] < SIZE - 40 and 40 < y[k] < SIZE - 40):
                continue
            if all((x[k] - cx) ** 2 + (y[k] - cy) ** 2 > 26 ** 2 for cx, cy in chosen):
                chosen.append((x[k], y[k]))
            if len(chosen) == 14:
                break
        with open(a.header, "w") as f:
            f.write("// Generated by tools/saver/render_glow.py - do not edit by hand.\n")
            f.write("// Screen positions of spots on the glow screensaver's tooth (/saver/glow.png).\n")
            f.write("#pragma once\n#include <stdint.h>\n\n")
            f.write("struct SaverSpot { int16_t x, y; };\n")
            f.write("static const SaverSpot SAVER_GLOW_SPOTS[] = {\n")
            for cx, cy in chosen:
                f.write("    { %d, %d },\n" % (round(cx), round(cy)))
            f.write("};\nstatic const int SAVER_GLOW_SPOT_COUNT = %d;\n" % len(chosen))
        print("wrote", a.header, len(chosen), "spots")

if __name__ == "__main__":
    main()

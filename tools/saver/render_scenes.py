#!/usr/bin/env python3
"""Kevin's Orb - render the screensaver's original scene pictures.

Three original pictures, painted entirely by this script (no photos, nobody else's art):

  cosmic  a nebula in magenta, violet, blue and gold, a ringed planet and a field of stars
  ocean   underwater: light rays from the surface, glowing jellyfish, coral on the sea floor
  aurora  northern lights in cyan, violet and pink over mountains, mirrored in a still lake

Colours keep away from red-against-green (Dr. Hollar is red/green colour blind): the aurora
is painted in its blue, violet and pink forms rather than the usual green.

Each is 466 x 466, rounded to the screen's 16-bit colour (RGB565) with dithering. The script
also writes src/saver_scenes.h: for each scene, the points the firmware animates (stars that
twinkle, jellyfish that pulse).

Usage:  python3 tools/saver/render_scenes.py OUTDIR [--header src/saver_scenes.h] [--only cosmic]
Needs numpy, scipy and Pillow.
"""
import argparse, os, sys
import numpy as np
from PIL import Image
from scipy import ndimage

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from render_glow import smooth, to_rgb565_dithered   # noqa: E402

SIZE = 466
SS = 2
N = SIZE * SS
YY, XX = np.mgrid[:N, :N] / N          # 0..1 coordinates, y down

def fbm(rng, octaves=((60, 1.0), (28, 0.55), (12, 0.3), (5, 0.18), (2, 0.08))):
    """Smooth fractal noise in about [0, 1]."""
    out = np.zeros((N, N))
    for sigma, amp in octaves:
        f = ndimage.gaussian_filter(rng.standard_normal((N, N)), sigma * SS / 2, mode="wrap")
        out += amp * f / (f.std() + 1e-9)
    out = (out - out.min()) / (out.max() - out.min())
    return out

def warp(img, dx, dy):
    coords = np.array([np.clip(YY * N + dy, 0, N - 1), np.clip(XX * N + dx, 0, N - 1)])
    if img.ndim == 2:
        return ndimage.map_coordinates(img, coords, order=1)
    return np.stack([ndimage.map_coordinates(img[..., c], coords, order=1) for c in range(img.shape[2])], -1)

def blob(cx, cy, r, soft):
    d = np.sqrt((XX * N - cx) ** 2 + (YY * N - cy) ** 2)
    return smooth(r, r - soft, d)

def stars(rng, count, bright, region=None, spikes=0):
    """A star field: returns (light, list of bright star positions in final pixels)."""
    img = np.zeros((N, N, 3))
    pts = []
    tints = [(1.0, 1.0, 1.0), (0.75, 0.85, 1.0), (1.0, 0.92, 0.78), (0.9, 0.8, 1.0)]
    for i in range(count):
        x, y = rng.random() * N, rng.random() * N
        if region is not None and not region(x / N, y / N):
            continue
        xi, yi = int(x), int(y)
        b = rng.random() ** 3 * 0.9 + 0.08
        img[yi, xi] += np.array(tints[rng.integers(4)]) * b
    img = ndimage.gaussian_filter(img, (0.7, 0.7, 0))
    img *= 3.0
    for i in range(bright):
        while True:
            x, y = rng.random(), rng.random()
            if region is None or region(x, y):
                break
        col = np.array(tints[rng.integers(4)])
        r = (1.6 + rng.random() * 1.6) * SS
        img += blob(x * N, y * N, r, r * 0.8)[..., None] * col
        img += ndimage.gaussian_filter(blob(x * N, y * N, r * 0.6, r * 0.5), 6 * SS)[..., None] * col * 2.5
        if spikes:
            ln = (10 + rng.random() * 14) * SS
            dx = np.abs(XX * N - x * N); dy = np.abs(YY * N - y * N)
            sp = np.exp(-dy / (0.6 * SS)) * np.clip(1 - dx / ln, 0, 1) ** 2 \
                + np.exp(-dx / (0.6 * SS)) * np.clip(1 - dy / ln, 0, 1) ** 2
            img += sp[..., None] * col * 0.5
        pts.append((x * SIZE, y * SIZE))
    return img, pts

def bloom(img, emis, a=0.7, b=0.7):
    return img + ndimage.gaussian_filter(emis, (4 * SS, 4 * SS, 0)) * a \
        + ndimage.gaussian_filter(emis, (16 * SS, 16 * SS, 0)) * b

def finish(img, rng, out):
    img = 1 - np.exp(-np.clip(img, 0, None) * 1.2)
    img = img.reshape(SIZE, SS, SIZE, SS, 3).mean(axis=(1, 3))
    yy, xx = np.mgrid[:SIZE, :SIZE]
    r = np.sqrt((xx - SIZE / 2 + 0.5) ** 2 + (yy - SIZE / 2 + 0.5) ** 2) / (SIZE / 2)
    img *= (1 - 0.5 * smooth(0.72, 1.0, r))[..., None]
    Image.fromarray(to_rgb565_dithered(np.clip(img, 0, 1), rng), "RGB").save(out, optimize=True)
    print("wrote", out, os.path.getsize(out), "bytes")

def pal(t, stops):
    """Colour ramp: stops = [(pos, (r,g,b)), ...]."""
    t = np.clip(t, 0, 1)
    out = np.zeros(t.shape + (3,))
    for (p0, c0), (p1, c1) in zip(stops[:-1], stops[1:]):
        m = (t >= p0) & (t <= p1)
        k = ((t - p0) / (p1 - p0))[m][:, None]
        out[m] = np.array(c0) * (1 - k) + np.array(c1) * k
    return out

# ================================================================ cosmic
def cosmic(rng):
    img = np.zeros((N, N, 3)) + np.array([0.008, 0.006, 0.02])
    n1, n2, n3 = fbm(rng), fbm(rng), fbm(rng)
    wx = (fbm(rng) - 0.5) * 90 * SS; wy = (fbm(rng) - 0.5) * 90 * SS
    n1 = warp(n1, wx, wy); n2 = warp(n2, wy, wx)
    # the nebula sits on a diagonal band from upper right to the middle left
    band = np.exp(-(((XX - 0.55) * 0.8 + (YY - 0.42) * 0.6) / 0.33) ** 2 * 1.0) \
        * np.exp(-(((XX - 0.55) * 0.6 - (YY - 0.42) * 0.8) / 0.55) ** 2)
    dens = smooth(0.35, 0.85, n1) * band
    col = pal(n2, [(0.0, (0.25, 0.10, 0.60)), (0.35, (0.75, 0.15, 0.65)), (0.6, (0.25, 0.45, 1.0)),
                   (0.8, (0.35, 0.80, 1.0)), (1.0, (1.0, 0.80, 0.45))])
    neb = col * dens[..., None] * 1.5
    # a hot gold-white core
    core = np.exp(-(((XX - 0.63) / 0.09) ** 2 + ((YY - 0.33) / 0.07) ** 2)) * smooth(0.3, 0.8, n3)
    neb += np.array([1.0, 0.78, 0.5]) * core[..., None] * 1.6
    # dark dust lanes
    dust = smooth(0.55, 0.8, warp(n3, wx * 0.5, wy * 0.5)) * band
    neb *= (1 - 0.75 * dust)[..., None]
    img += neb
    st, pts = stars(rng, 2600, 16, spikes=1)
    img += st * (1 - 0.6 * dust)[..., None]
    emis = neb * 0.5 + st * 0.6
    # ringed planet, lower left
    pcx, pcy, pr = 0.36 * N, 0.70 * N, 0.14 * N
    dx = (XX * N - pcx) / pr; dy = (YY * N - pcy) / pr
    d2 = dx * dx + dy * dy
    inside = d2 < 1
    nz = np.sqrt(np.clip(1 - d2, 0, 1))
    L = np.array([0.75, -0.45, 0.5]); L /= np.linalg.norm(L)       # lit from the nebula side
    lam = np.clip(dx * L[0] + dy * L[1] + nz * L[2], 0, 1)
    tilt = dy * 0.95 + dx * 0.32
    bands = 0.5 + 0.5 * np.sin(tilt * 15 + np.sin(tilt * 5 + dx * 2) * 1.5)
    pcol = pal(bands, [(0, (0.20, 0.25, 0.70)), (0.5, (0.45, 0.35, 0.85)), (1, (0.85, 0.65, 0.95))])
    rimv = (1 - nz) ** 3
    planet = pcol * (0.04 + 1.0 * lam)[..., None] + np.array([0.5, 0.4, 1.0]) * (rimv * lam ** 0.3)[..., None] * 0.8
    # ring: an ellipse around the planet, tilted
    ang = np.radians(-18)
    rx = (XX * N - pcx) * np.cos(ang) - (YY * N - pcy) * np.sin(ang)
    ry = (XX * N - pcx) * np.sin(ang) + (YY * N - pcy) * np.cos(ang)
    rr = np.sqrt((rx / (pr * 2.1)) ** 2 + (ry / (pr * 0.42)) ** 2)
    ringm = smooth(0.62, 0.66, rr) * smooth(1.0, 0.96, rr) * (0.55 + 0.45 * np.sin(rr * 70) ** 2)
    ringm *= np.where((rr > 0.78) & (rr < 0.81), 0.15, 1.0)            # a gap
    ringc = np.array([0.95, 0.85, 0.75]) * 0.9
    front = ry > 0                                                    # nearer half of the ring
    soft = smooth(1.0, 0.985, np.sqrt(d2))
    behind = ringm * (~front)
    img = img * (1 - behind[..., None] * 0.9) + ringc * behind[..., None]
    img = img * (1 - soft[..., None]) + planet * soft[..., None]
    # ring shadow on the planet
    img *= (1 - 0.5 * soft * smooth(0.55, 0.62, rr) * smooth(1.05, 0.95, rr) * (~front))[..., None]
    frontm = ringm * front
    # the planet's shadow across the ring's far side is behind it; on the near side, the
    # part of the ring that crosses the planet's dark side is darker
    shade = np.where(inside, 0.55 + 0.45 * lam, 1.0)
    img = img * (1 - frontm[..., None] * 0.9) + (ringc * shade[..., None]) * frontm[..., None]
    emis = emis * (1 - soft[..., None])
    # a small moon
    mcx, mcy, mr = 0.76 * N, 0.76 * N, 0.032 * N
    mdx = (XX * N - mcx) / mr; mdy = (YY * N - mcy) / mr
    md2 = mdx ** 2 + mdy ** 2
    mz = np.sqrt(np.clip(1 - md2, 0, 1))
    mlam = np.clip(mdx * L[0] + mdy * L[1] + mz * L[2], 0, 1)
    ms = smooth(1.0, 0.94, np.sqrt(md2))
    img = img * (1 - ms[..., None]) + (np.array([0.75, 0.75, 0.85]) * (0.03 + mlam))[..., None].squeeze(-1)[..., None] * ms[..., None] if False else \
        img * (1 - ms[..., None]) + (np.array([0.75, 0.75, 0.85])[None, None, :] * (0.03 + mlam)[..., None]) * ms[..., None]
    img = bloom(img, emis)
    # twinkle points: bright stars away from the planet
    pts = [(x, y) for x, y in pts if (x - pcx / SS) ** 2 + (y - pcy / SS) ** 2 > (pr / SS * 1.3) ** 2]
    return img, pts

# ================================================================ ocean
def ocean(rng):
    depth = YY
    water = pal(depth, [(0.0, (0.10, 0.45, 0.80)), (0.25, (0.05, 0.25, 0.60)),
                        (0.6, (0.03, 0.10, 0.35)), (1.0, (0.02, 0.04, 0.16))])
    img = water.copy()
    # the surface: bright rippled ceiling at the top
    rip = fbm(rng, ((14, 1.0), (6, 0.6), (2, 0.3)))
    surf = smooth(0.13, 0.0, YY) * (0.4 + 0.8 * smooth(0.45, 0.8, rip))
    img += np.array([0.45, 0.80, 1.0]) * surf[..., None] * 0.9
    # light rays fanning down from a point above the top
    ang = np.arctan2(XX - 0.55, YY + 0.35)
    rays = np.zeros((N, N))
    for k in range(14):
        a0 = (rng.random() - 0.5) * 0.9
        w = 0.008 + rng.random() * 0.025
        rays += np.exp(-((ang - a0) / w) ** 2) * (0.3 + rng.random() * 0.7)
    rays *= smooth(0.95, 0.05, YY) ** 1.5
    img += np.array([0.40, 0.75, 1.0]) * rays[..., None] * 0.35
    # caustics on the sand
    caus = np.abs(np.sin(warp(fbm(rng, ((8, 1), (3, 0.5))), 0, 0) * 40))
    caus = smooth(0.85, 1.0, 1 - caus)
    # sea floor: soft dunes
    xs = XX[0]
    floor_y = 0.80 + 0.04 * np.sin(xs * 6.0 + 1.0) + 0.02 * np.sin(xs * 17.0)
    fm = smooth(-0.004, 0.004, YY - floor_y[None, :])
    sand = pal(YY, [(0.75, (0.40, 0.32, 0.45)), (1.0, (0.16, 0.10, 0.25))]) * (0.7 + 0.3 * fbm(rng, ((2, 1), (1, 0.5))))[..., None]
    sand += np.array([0.5, 0.7, 1.0]) * (caus * 0.25)[..., None]
    img = img * (1 - fm[..., None]) + sand * fm[..., None]
    emis = np.zeros((N, N, 3))
    # coral: branching strokes from the floor, in pink, violet, orange, gold
    cols = [(1.0, 0.45, 0.75), (0.75, 0.45, 1.0), (1.0, 0.60, 0.30), (1.0, 0.82, 0.40), (0.55, 0.70, 1.0)]
    coral = np.zeros((N, N, 3)); cm = np.zeros((N, N))
    disk = {}
    def stamp(buf, x, y, r, val, mode="set"):
        """Paint a filled circle, clipped to the picture (no negative-index wrap-around)."""
        r = max(1, int(r))
        if r not in disk:
            yy_, xx_ = np.mgrid[-r:r + 1, -r:r + 1]
            disk[r] = (xx_ ** 2 + yy_ ** 2) <= r * r
        k = disk[r]
        xi, yi = int(x), int(y)
        x0, x1, y0, y1 = xi - r, xi + r + 1, yi - r, yi + r + 1
        cx0, cy0 = max(0, x0), max(0, y0)
        cx1, cy1 = min(N, x1), min(N, y1)
        if cx0 >= cx1 or cy0 >= cy1:
            return
        kk = k[cy0 - y0:cy1 - y0, cx0 - x0:cx1 - x0]
        if mode == "add":
            buf[cy0:cy1, cx0:cx1][kk] += val
        else:
            buf[cy0:cy1, cx0:cx1][kk] = val
    def branch(x, y, a, ln, w, col, depth_):
        steps = int(ln / SS) + 2
        for s in range(steps):
            x += np.sin(a) * SS; y -= np.cos(a) * SS
            a += (rng.random() - 0.5) * 0.10
            ww = w * (1 - 0.35 * s / steps)
            stamp(cm, x, y, ww, 1.0)
            stamp(coral, x, y, ww, np.array(col))
        if depth_ > 0:
            for d in (-1, 1):
                branch(x, y, a + d * (0.30 + rng.random() * 0.35), ln * 0.7, w * 0.8, col, depth_ - 1)
        else:
            # a rounded, glowing polyp tip
            tipc = np.array(col) * 0.5 + 0.5
            stamp(cm, x, y, w * 1.5, 1.0)
            stamp(coral, x, y, w * 1.5, tipc)
            stamp(emis, x, y, w * 1.6, tipc * 0.9, "add")
    for ci, cx in enumerate((0.16, 0.25, 0.34, 0.68, 0.77, 0.86)):
        col = cols[(ci * 2 + 1) % len(cols)]
        fy = 0.80 + 0.04 * np.sin(cx * 6 + 1) + 0.02 * np.sin(cx * 17)
        for k in range(2):
            branch(cx * N + (rng.random() - 0.5) * 16 * SS, fy * N + 4 * SS, (rng.random() - 0.5) * 0.4,
                   (22 + rng.random() * 18) * SS, 5.0 * SS, col, 2)
    # brain-coral mounds on the floor
    for mx, mr, colr in ((0.47, 26, (0.80, 0.50, 1.0)), (0.58, 18, (1.0, 0.75, 0.40)), (0.08, 20, (1.0, 0.50, 0.80))):
        fy = 0.80 + 0.04 * np.sin(mx * 6 + 1) + 0.02 * np.sin(mx * 17)
        dx = (XX * N - mx * N) / (mr * SS); dy = (YY * N - (fy * N + 6 * SS)) / (mr * SS * 0.75)
        d = np.sqrt(dx ** 2 + dy ** 2)
        m = smooth(1.0, 0.93, d) * (dy < 0.2)
        ridges = 0.65 + 0.35 * np.sin(fbm(rng, ((3, 1), (1, 0.4))) * 60)
        mc = np.array(colr) * (ridges * (0.45 + 0.55 * smooth(0.6, -1.0, dy)))[..., None]
        cm = np.maximum(cm, m)
        coral = coral * (1 - m[..., None]) + mc * m[..., None]
    cm = ndimage.gaussian_filter(cm, 0.7 * SS)
    coral = ndimage.gaussian_filter(coral, (0.7 * SS, 0.7 * SS, 0))
    shade = 0.55 + 0.45 * smooth(0.95, 0.70, YY)
    img = img * (1 - cm[..., None]) + coral * (shade * 0.9)[..., None]
    # sea fan silhouettes behind (dark violet)
    # jellyfish: glowing bells with trailing tentacles
    pts = []
    jellies = [(0.62, 0.34, 34, (1.0, 0.50, 0.85)), (0.30, 0.46, 24, (0.60, 0.55, 1.0)),
               (0.80, 0.58, 16, (0.45, 0.85, 1.0))]
    for jx, jy, jr, col in jellies:
        cx, cy, r = jx * N, jy * N, jr * SS
        dx = (XX * N - cx) / r; dy = (YY * N - cy) / (r * 0.8)
        d = np.sqrt(dx ** 2 + dy ** 2)
        bell = smooth(1.0, 0.92, d) * (dy < 0.25)
        rim = np.exp(-((d - 0.92) / 0.08) ** 2) * (dy < 0.3)
        inner = smooth(0.9, 0.0, d) * 0.35
        lobes = 0.7 + 0.3 * np.sin(np.arctan2(dy, dx) * 8)
        jc = np.array(col)
        img = img * (1 - 0.55 * bell[..., None]) + jc * ((inner * lobes + 0.35) * bell)[..., None] * 0.9
        img += jc * rim[..., None] * 0.9
        emis += jc * (rim * 0.9 + bell * 0.35)[..., None]
        # tentacles
        tent = np.zeros((N, N))
        for t in range(7):
            x0 = cx + (t - 3) * r * 0.25
            ln = r * (2.2 + rng.random() * 1.6)
            ph = rng.random() * 6
            for s in range(int(ln)):
                yy_ = cy + r * 0.2 + s
                xx_ = x0 + np.sin(s / (8.0 * SS) + ph) * (2 + s * 0.05)
                xi, yi = int(xx_), int(yy_)
                if 0 <= xi < N and 0 <= yi < N:
                    tent[yi, xi] += 1.0 - s / ln
        tent = ndimage.gaussian_filter(tent, 0.7 * SS)
        img += jc * tent[..., None] * 0.6
        emis += jc * tent[..., None] * 0.4
        pts.append((jx * SIZE, jy * SIZE, jr))
    # marine snow
    snow = np.zeros((N, N))
    for i in range(500):
        x, y = int(rng.random() * N), int(rng.random() * N * 0.85)
        snow[y, x] = rng.random() * 0.8
    snow = ndimage.gaussian_filter(snow, 0.8 * SS) * 6
    img += np.array([0.6, 0.8, 1.0]) * snow[..., None] * 0.6
    img = bloom(img, emis, 0.8, 0.9)
    return img, pts

# ================================================================ aurora
def aurora(rng):
    horizon = 0.62
    sky = pal(YY / horizon, [(0.0, (0.02, 0.02, 0.09)), (0.6, (0.05, 0.04, 0.18)), (1.0, (0.16, 0.08, 0.28))])
    st, pts = stars(rng, 2200, 14, region=lambda x, y: y < horizon - 0.05, spikes=0)
    # aurora curtains: wavy base lines with tall vertical rays
    au = np.zeros((N, N, 3))
    xs = XX[0]
    curtains = [(0.34, 0.07, 1.0, 0.0), (0.22, 0.05, 0.75, 2.0), (0.40, 0.03, 0.45, 4.0)]
    for base, amp, strength, ph in curtains:
        y0 = base + amp * np.sin(xs * 4.0 + ph) + 0.035 * np.sin(xs * 11 + ph * 2) + 0.015 * np.sin(xs * 29 + ph)
        above = (y0[None, :] - YY)                     # >0 above the base line
        streak = ndimage.gaussian_filter1d(rng.random(N), 1.2 * SS, mode="wrap")
        streak = (streak - streak.min()) / (streak.max() - streak.min())
        streak = 0.35 + 0.65 * streak ** 1.5
        h = 0.13 + 0.07 * np.sin(xs * 3 + ph)
        # a bright lower edge, then rays fading upward
        prof = np.where(above > 0, 0.55 * np.exp(-above / h[None, :]) + 0.9 * np.exp(-above / 0.015),
                        np.exp(above / 0.005))
        inten = prof * streak[None, :] * smooth(0.0, 0.25, xs)[None, :] * smooth(1.0, 0.75, xs)[None, :] * strength
        # colour by height above the base: cyan at the bottom, violet, pink at the top
        t = np.clip(above / (h[None, :] * 2.2), 0, 1)
        c = pal(t, [(0.0, (0.45, 0.95, 1.0)), (0.35, (0.35, 0.55, 1.0)), (0.7, (0.70, 0.40, 1.0)), (1.0, (1.0, 0.45, 0.80))])
        au += c * inten[..., None]
    au = ndimage.gaussian_filter(au, (0.8 * SS, 0.4 * SS, 0))
    sky_img = sky + st * (1 - np.clip(au.sum(-1), 0, 1))[..., None] + au * 1.1
    # mountains: two ranges
    img = sky_img.copy()
    rng_n = ndimage.gaussian_filter1d(rng.standard_normal(N), 18 * SS)
    rng_n2 = ndimage.gaussian_filter1d(rng.standard_normal(N), 9 * SS)
    def peaks(x, k, off):
        t = (x * k + off) % 1.0
        return (1 - np.abs(t * 2 - 1)) ** 1.6
    far = horizon - 0.05 - 0.11 * peaks(xs, 2.3, 0.15) - 0.05 * peaks(xs, 5.1, 0.6) \
        - rng_n / np.abs(rng_n).max() * 0.012
    near = horizon - 0.04 - 0.07 * np.abs(np.sin(xs * 4.7 + 2.0)) - rng_n2 / np.abs(rng_n2).max() * 0.03
    farm = smooth(-0.002, 0.002, YY - far[None, :])
    # snow on the higher slopes only, lit faintly by the aurora
    high = smooth(horizon - 0.10, horizon - 0.15, YY)
    snow = smooth(0.03, 0.0, YY - far[None, :]) * high
    farcol = np.array([0.05, 0.05, 0.14]) + np.array([0.45, 0.55, 0.85]) * snow[..., None] * 0.55
    img = img * (1 - farm[..., None]) + farcol * farm[..., None]
    nearm = smooth(-0.002, 0.002, YY - near[None, :])
    img = img * (1 - nearm[..., None]) + np.array([0.035, 0.03, 0.09]) * nearm[..., None]
    # pines along the near shore
    trees = np.zeros((N, N))
    for i in range(70):
        tx = rng.random()
        th = 0.025 + rng.random() * 0.045
        ty = horizon
        dxp = np.abs(XX - tx)
        hgt = ty - YY
        tri = (hgt > 0) & (hgt < th) & (dxp < (th - hgt) * 0.28 * (1 + 0.25 * np.sin(hgt * 600)))
        trees = np.maximum(trees, tri)
    img = img * (1 - trees[..., None]) + np.array([0.02, 0.02, 0.05]) * trees[..., None]
    # lake: mirrored top half with ripples, darker and bluer
    lake = YY > horizon
    my = np.clip(2 * horizon - YY, 0, 1)
    ripple = np.sin((YY - horizon) * 260 / np.clip(YY - horizon + 0.05, 0.01, None) ** 0.3) * 0.6 * SS * smooth(0.0, 0.3, YY - horizon)
    coords = np.array([my * N, np.clip(XX * N + ripple, 0, N - 1)])
    refl = np.stack([ndimage.map_coordinates(img[..., c], coords, order=1) for c in range(3)], -1)
    refl *= 0.55
    refl += np.array([0.02, 0.03, 0.08])
    img = np.where(lake[..., None], refl, img)
    # a faint shoreline glint
    img += np.array([0.3, 0.5, 0.9]) * np.exp(-((YY - horizon) / 0.003) ** 2)[..., None] * 0.15
    emis = au * 0.6 + st * 0.5 * (YY < horizon)[..., None]
    img = bloom(img, emis, 0.6, 0.6)
    return img, pts

SCENES = {"cosmic": cosmic, "ocean": ocean, "aurora": aurora}

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("outdir")
    ap.add_argument("--header")
    ap.add_argument("--only")
    ap.add_argument("--seed", type=int, default=11)
    a = ap.parse_args()
    os.makedirs(a.outdir, exist_ok=True)
    allpts = {}
    for i, (name, fn) in enumerate(SCENES.items()):
        if a.only and name != a.only:
            continue
        rng = np.random.default_rng(a.seed + i * 101)
        img, pts = fn(rng)
        finish(img, rng, os.path.join(a.outdir, name + ".png"))
        allpts[name] = pts
    if a.header:
        with open(a.header, "w") as f:
            f.write("// Generated by tools/saver/render_scenes.py - do not edit by hand.\n")
            f.write("// The points each screensaver scene animates: stars that twinkle (x, y, 0) and\n")
            f.write("// jellyfish that pulse (x, y, bell radius). Pictures: /saver/<name>.png on the SD card.\n")
            f.write("#pragma once\n#include <stdint.h>\n\nstruct SaverPoint { int16_t x, y, r; };\n\n")
            for name, pts in allpts.items():
                f.write("static const SaverPoint SAVER_%s_POINTS[] = {\n" % name.upper())
                for p in pts:
                    f.write("    { %d, %d, %d },\n" % (round(p[0]), round(p[1]), p[2] if len(p) > 2 else 0))
                f.write("};\n\n")
        print("wrote", a.header)

if __name__ == "__main__":
    main()

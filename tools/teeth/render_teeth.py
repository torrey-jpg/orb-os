#!/usr/bin/env python3
"""Kevin's Orb - render the Tooth Explorer's 3D teeth.

Each of the 16 tooth types (upper and lower: third molar .. central incisor) is built as a
signed distance field from simple shapes - a crown body, cusps and tapered roots - and
ray-marched on true black from every direction: 24 angles around the tooth's long axis (yaw)
times 24 angles end over end (pitch), 15 degrees apart, so it can be turned any way. The
frames for one tooth are packed into one file, teeth/tNN.bin; the firmware reads the table
once and then only the frame it is showing:

    "TTH2"  u16 yaw_count  u16 pitch_count  u16 width  u16 height
    yaw_count*pitch_count x (u32 offset, u32 length)   frame index = pitch*yaw_count + yaw
    the PNGs, back to back

Shapes are simplified (crown outline, cusp count and root count follow standard dental
anatomy; sizes are textbook averages in mm). They are placeholders until licensed anatomical
models replace them - same file format, no firmware change.

Usage:  python3 render_teeth.py OUTDIR [--types 0,7] [--yaw 24] [--pitch 24] [--size 220]
Needs numpy and Pillow.
"""
import argparse, io, os, struct, sys
from concurrent.futures import ProcessPoolExecutor
import numpy as np
from PIL import Image

# ---------------------------------------------------------------- SDF primitives
def length(v):
    return np.sqrt(np.sum(v * v, axis=-1))

def smin(a, b, k):
    h = np.clip(0.5 + 0.5 * (b - a) / k, 0.0, 1.0)
    return b * (1 - h) + a * h - k * h * (1 - h)

def smax(a, b, k):
    return -smin(-a, -b, k)

def ellipsoid(p, c, r):
    q = (p - c) / r
    k0 = length(q)
    k1 = length(q / r)
    return k0 * (k0 - 1.0) / np.maximum(k1, 1e-6)

def sphere(p, c, r):
    return length(p - c) - r

def cone_capsule(p, a, b, ra, rb):
    """Round cone between points a (radius ra) and b (radius rb)."""
    ba = b - a
    pa = p - a
    t = np.clip(np.sum(pa * ba, axis=-1) / np.dot(ba, ba), 0.0, 1.0)
    r = ra + (rb - ra) * t
    return length(pa - t[..., None] * ba) - r

# ---------------------------------------------------------------- tooth types
# (name, upper?, crown height, mesiodistal, buccolingual, root length, kind)
TYPES = [
    ("Upper Third Molar",     True,  6.5,  8.5, 10.0, 11.0, "m3u"),
    ("Upper Second Molar",    True,  7.0,  9.0, 11.0, 12.0, "m3r"),
    ("Upper First Molar",     True,  7.5, 10.0, 11.0, 13.0, "m3r"),
    ("Upper Second Premolar", True,  8.5,  6.5,  9.0, 14.0, "pm1"),
    ("Upper First Premolar",  True,  8.5,  7.0,  9.0, 14.0, "pm2"),
    ("Upper Canine",          True, 10.0,  7.5,  8.0, 17.0, "can"),
    ("Upper Lateral Incisor", True,  9.0,  6.5,  6.0, 13.0, "inc"),
    ("Upper Central Incisor", True, 10.5,  8.5,  7.0, 13.0, "inc"),
    ("Lower Third Molar",     False, 7.0, 10.0,  9.5, 11.0, "m2f"),
    ("Lower Second Molar",    False, 7.0, 10.5, 10.0, 13.0, "m2r"),
    ("Lower First Molar",     False, 7.5, 11.0, 10.5, 14.0, "m2r5"),
    ("Lower Second Premolar", False, 8.0,  7.0,  8.0, 14.5, "pm1"),
    ("Lower First Premolar",  False, 8.5,  7.0,  7.5, 14.0, "pm1s"),
    ("Lower Canine",          False, 11.0, 7.0,  7.5, 16.0, "can"),
    ("Lower Lateral Incisor", False, 9.5,  5.5,  6.5, 14.0, "inc"),
    ("Lower Central Incisor", False, 9.0,  5.0,  6.0, 12.5, "inc"),
]

V = lambda *a: np.array(a, dtype=np.float64)

def tooth_sdf(p, t):
    """p: (...,3) in tooth space, mm. y up = toward the biting surface, cervix at y = 0.
    x = mesiodistal, z = buccolingual (z+ faces the cheek/lip)."""
    _, _, ch, md, bl, rl, kind = t
    x, y, z = p[..., 0], p[..., 1], p[..., 2]
    hx, hz = md / 2, bl / 2
    yn = np.clip(y / ch, 0.0, 1.0)

    if kind in ("inc", "can"):
        # Blade: widens a little toward the edge, thins toward the edge.
        wx = hx * (0.72 + 0.28 * np.sqrt(yn))
        wz = hz * (1.0 - (0.78 if kind == "inc" else 0.55) * yn ** 1.4)
        q = np.stack([x / wx * hx, y, z / np.maximum(wz, 0.25) * hz], axis=-1)
        crown = ellipsoid(q, V(0, ch * 0.42, 0), V(hx, ch * 0.62, hz)) * 0.8
        if kind == "inc":
            crown = smax(crown, y - ch, 0.5)                          # flat incisal edge
        else:
            crown = smax(crown, y - (ch - 0.9 * np.abs(x)), 0.6)      # pointed cusp
        roots = cone_capsule(p, V(0, 0.5, 0), V(0.0, -rl + 1.0, -0.4), min(hx, hz) * 0.88, 1.0)

    elif kind.startswith("pm"):
        body = ellipsoid(p, V(0, ch * 0.42, 0), V(hx * 0.95, ch * 0.55, hz * 0.95))
        lr = 0.55 if kind == "pm1s" else 0.85                          # lingual cusp size
        cb = sphere(p, V(0, ch * 0.80, hz * 0.30), hx * 0.62)
        cl = sphere(p, V(0, ch * 0.72 - (1 - lr) * 2, -hz * 0.32), hx * 0.62 * lr)
        crown = smin(smin(body, cb, 1.2), cl, 1.2)
        groove = cone_capsule(p, V(-hx, ch * 1.02, -0.2), V(hx, ch * 1.02, -0.2), 0.75, 0.75)
        crown = smax(crown, -groove, 0.5)
        crown = smax(crown, y - ch * 1.05, 0.5)
        if kind == "pm2":   # upper first premolar: buccal and palatal roots
            roots = smin(cone_capsule(p, V(0, 0.5, hz * 0.35), V(0, -rl + 0.8, hz * 0.55), hx * 0.66, 0.8),
                         cone_capsule(p, V(0, 0.5, -hz * 0.35), V(0, -rl * 0.95 + 0.8, -hz * 0.6), hx * 0.64, 0.8), 1.0)
        else:
            roots = cone_capsule(p, V(0, 0.5, 0), V(0.3, -rl + 1.0, 0), hx * 0.85, 1.0)

    else:  # molars
        body = ellipsoid(p, V(0, ch * 0.40, 0), V(hx * 1.0, ch * 0.52, hz * 0.98))
        if kind == "m2r5":      # lower first molar: 3 buccal + 2 lingual cusps
            cusps = [(-0.55, 0.40), (0.0, 0.44), (0.58, 0.36), (-0.42, -0.42), (0.42, -0.42)]
        elif kind.startswith("m2"):  # lower: 2 buccal + 2 lingual
            cusps = [(-0.45, 0.40), (0.45, 0.40), (-0.45, -0.42), (0.45, -0.42)]
        else:                   # upper: MB, DB, MP (largest), DP
            cusps = [(-0.45, 0.42), (0.45, 0.42), (-0.40, -0.38), (0.48, -0.45)]
        crown = body
        for i, (cx, cz) in enumerate(cusps):
            r = min(hx, hz) * (0.50 if (kind.startswith("m3") and i == 2) else 0.42)
            crown = smin(crown, sphere(p, V(cx * hx, ch * 0.80, cz * hz), r), 1.1)
        crown = smax(crown, -sphere(p, V(0, ch * 1.12, 0), min(hx, hz) * 0.42), 0.6)  # central fossa
        crown = smax(crown, y - ch * 1.05, 0.4)
        if kind in ("m3u", "m2f"):   # third molars: short, converging, partly fused roots
            if kind == "m3u":
                pts = [V(-hx * 0.3, -rl, hz * 0.1), V(hx * 0.2, -rl * 0.9, hz * 0.2), V(0, -rl * 0.9, -hz * 0.25)]
            else:
                pts = [V(-hx * 0.15, -rl, 0), V(hx * 0.25, -rl * 0.9, 0)]
            roots = None
            for tip in pts:
                r_ = cone_capsule(p, V(tip[0] * 0.5, 0.5, tip[2] * 0.5), tip, hx * 0.6, 1.0)
                roots = r_ if roots is None else smin(roots, r_, 1.4)
        elif kind.startswith("m3"):  # upper molar: two buccal roots + a large palatal root
            roots = smin(cone_capsule(p, V(-hx * 0.45, 0.5, hz * 0.45), V(-hx * 0.55, -rl * 0.92, hz * 0.55), 2.0, 0.85),
                         cone_capsule(p, V(hx * 0.45, 0.5, hz * 0.45), V(hx * 0.6, -rl * 0.88, hz * 0.55), 1.9, 0.85), 0.6)
            roots = smin(roots, cone_capsule(p, V(0, 0.5, -hz * 0.4), V(0, -rl, -hz * 0.85), 2.4, 1.0), 0.6)
        else:                        # lower molar: mesial and distal roots, slight distal curve
            roots = smin(cone_capsule(p, V(-hx * 0.5, 0.5, 0), V(-hx * 0.35, -rl, 0), 2.4, 0.9),
                         cone_capsule(p, V(hx * 0.5, 0.5, 0), V(hx * 0.75, -rl * 0.92, 0), 2.3, 0.9), 0.6)
        trunk = ellipsoid(p, V(0, -0.5, 0), V(hx * 0.78, 2.6, hz * 0.76))
        roots = smin(roots, trunk, 1.0)

    return smin(crown, roots, 1.0)

# ---------------------------------------------------------------- renderer
SCALE_MM = 30.0   # mm across the frame: one scale for every tooth, so sizes compare honestly

def rot_y(a):
    c, s = np.cos(a), np.sin(a)
    return np.array([[c, 0, s], [0, 1, 0], [-s, 0, c]])

def rot_x(a):
    c, s = np.cos(a), np.sin(a)
    return np.array([[1, 0, 0], [0, c, -s], [0, s, c]])

def render(t, yaw_deg, pitch_deg, size, ss=2):
    """Orbit camera: yaw spins the tooth about its own long axis, pitch then tumbles it end
    over end about the screen's horizontal axis. Pitch 0 is the tooth upright, seen from the
    cheek side with the camera level."""
    upper = t[1]
    ch, rl, md, bl = t[2], t[5], t[3], t[4]
    n = size * ss
    mid = (ch - rl) / 2.0
    u = (np.arange(n) + 0.5) / n - 0.5
    px, py = np.meshgrid(u * SCALE_MM, -u * SCALE_MM)
    R = rot_x(np.radians(pitch_deg)) @ rot_y(np.radians(yaw_deg))   # model -> camera
    flip = np.diag([1.0, -1.0 if upper else 1.0, 1.0])           # upper teeth: crown down
    M = flip @ R.T                                                 # camera -> model
    o = np.stack([px, py, np.full_like(px, 40.0)], axis=-1).reshape(-1, 3)
    O = o @ M.T + np.array([0.0, mid, 0.0])
    D = M @ np.array([0.0, 0.0, -1.0])

    # Only march rays that can hit the tooth: its bounding sphere, entered at the near side.
    c = np.array([0.0, mid, 0.0])
    rad = np.sqrt(((ch + rl) / 2 + 1.5) ** 2 + (max(md, bl) / 2 + 2.0) ** 2)
    oc = O - c
    b = oc @ D
    disc = b * b - (np.sum(oc * oc, axis=1) - rad * rad)
    cand = np.nonzero(disc > 0)[0]
    tt = np.zeros(O.shape[0])
    tt[cand] = -b[cand] - np.sqrt(disc[cand])
    tmax = np.zeros(O.shape[0])
    tmax[cand] = -b[cand] + np.sqrt(disc[cand])
    hit = np.zeros(O.shape[0], dtype=bool)
    alive = np.zeros(O.shape[0], dtype=bool)
    alive[cand] = True
    for _ in range(140):
        idx = np.nonzero(alive)[0]
        if idx.size == 0:
            break
        pos = O[idx] + tt[idx, None] * D
        dist = tooth_sdf(pos, t)
        tt[idx] += dist * 0.6
        done = dist < 0.01
        hit[idx[done]] = True
        far = tt[idx] > tmax[idx]
        alive[idx[done | far]] = False

    img = np.zeros((O.shape[0], 3))
    hi = np.nonzero(hit)[0]
    if hi.size:
        P = O[hi] + tt[hi, None] * D
        e = 0.02
        nrm = np.stack([tooth_sdf(P + V(e, 0, 0), t) - tooth_sdf(P - V(e, 0, 0), t),
                        tooth_sdf(P + V(0, e, 0), t) - tooth_sdf(P - V(0, e, 0), t),
                        tooth_sdf(P + V(0, 0, e), t) - tooth_sdf(P - V(0, 0, e), t)], axis=-1)
        nrm /= np.maximum(length(nrm)[:, None], 1e-9)
        # Lighting is fixed to the camera, so it stays put while the tooth turns.
        Nc = nrm @ M
        L1 = V(-0.5, 0.6, 0.65); L1 /= np.linalg.norm(L1)
        L2 = V(0.7, -0.2, 0.5);  L2 /= np.linalg.norm(L2)
        Vw = V(0, 0, 1)
        dif = np.clip(Nc @ L1, 0, 1) * 0.85 + np.clip(Nc @ L2, 0, 1) * 0.25 + 0.12
        Hh = (L1 + Vw); Hh /= np.linalg.norm(Hh)
        spec = np.clip(Nc @ Hh, 0, 1) ** 40
        fres = (1 - np.clip(Nc @ Vw, 0, 1)) ** 3
        y = P[:, 1]                            # model space: >0 crown, <0 root
        k = np.clip((y + 0.3) / 1.2, 0, 1)[:, None]
        enamel = V(0.96, 0.94, 0.88)
        root = V(0.88, 0.80, 0.66)
        base = root * (1 - k) + enamel * k
        gloss = (0.15 + 0.55 * k[:, 0])
        mint = V(0.37, 0.89, 0.75)
        col = base * dif[:, None] + spec[:, None] * gloss[:, None] + fres[:, None] * mint * 0.55
        img[hi] = col
    img = np.clip(img, 0, 1) ** (1 / 1.1)
    img = img.reshape(n, n, 3)
    if ss > 1:
        img = img.reshape(size, ss, size, ss, 3).mean(axis=(1, 3))
    return Image.fromarray((img * 255 + 0.5).astype(np.uint8), "RGB")

def build(args):
    """One tooth type -> teeth/tNN.bin (format in the module docstring)."""
    i, outdir, nyaw, npitch, size = args
    t = TYPES[i]
    pngs = []
    for p_ in range(npitch):
        for y_ in range(nyaw):
            im = render(t, y_ * 360.0 / nyaw, p_ * 360.0 / npitch, size)
            b = io.BytesIO()
            im.save(b, "PNG", optimize=True)
            pngs.append(b.getvalue())
    head = struct.pack("<4sHHHH", b"TTH2", nyaw, npitch, size, size)
    off = len(head) + 8 * len(pngs)
    table = b""
    for d in pngs:
        table += struct.pack("<II", off, len(d))
        off += len(d)
    tmp = os.path.join(outdir, f"t{i:02d}.bin.part")
    with open(tmp, "wb") as fh:
        fh.write(head + table + b"".join(pngs))
    os.replace(tmp, os.path.join(outdir, f"t{i:02d}.bin"))
    return i, t[0], sum(len(d) for d in pngs)

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("outdir")
    ap.add_argument("--types", default=",".join(str(i) for i in range(16)))
    ap.add_argument("--yaw", type=int, default=24, help="angles around the long axis")
    ap.add_argument("--pitch", type=int, default=24, help="angles end over end")
    ap.add_argument("--size", type=int, default=220)
    ap.add_argument("--jobs", type=int, default=os.cpu_count())
    a = ap.parse_args()
    os.makedirs(a.outdir, exist_ok=True)
    jobs = [(int(i), a.outdir, a.yaw, a.pitch, a.size) for i in a.types.split(",")]
    with ProcessPoolExecutor(max_workers=a.jobs) as ex:
        for i, name, nbytes in ex.map(build, jobs):
            print(f"t{i:02d} {name:24s} {nbytes/1024:7.0f} KB")

if __name__ == "__main__":
    main()

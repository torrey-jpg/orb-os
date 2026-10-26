#!/usr/bin/env python3
"""Kevin's Orb - the Bateman Dental steampunk clock, rendered with depth.

The dial seen head-on is a stack of raised parts, so it is built as a height field: every
part (back plate, gears, gauges, chapter ring, bezel, rivets, name plate) has a shape, a
height profile with bevelled edges and a metal. The renderer then lights the stack from the
upper left with real shadows (each pixel looks toward the light across the height field),
ambient occlusion in the crevices, metallic highlights and a soft reflection.

The hands are rendered the same way, without the shadow pass (the Orb's firmware draws hand
shadows itself), and a glass layer (clock_overlay.png) adds the domed crystal over the top.

Writes the theme folder the firmware reads (the same layout Orb Studio installs):
  OUTDIR/bateman-steampunk/clock_plate.png, clock_overlay.png, clock_hand_*.png,
  clock_shadow_*.png, clock_style.json, theme.json, _installed

Usage: python3 render_steampunk3d.py OUTDIR [--name "BATEMAN DENTAL"]
       [--frames 12] [--fps 4]   (the turning-gear loop; --frames 1 for a still dial)
Needs numpy, scipy and Pillow. Original artwork, drawn in code.
"""
import argparse, json, math, os
import numpy as np
from PIL import Image, ImageDraw, ImageFilter, ImageFont
from scipy import ndimage

W = 466
SS = 2
N = W * SS
C = W / 2
SLUG = "bateman-steampunk"
LOOP_TEETH = 2      # teeth each gear advances per loop of the moving background
SERIF = "/usr/share/fonts/truetype/liberation/LiberationSerif-Bold.ttf"
SANS = "/usr/share/fonts/truetype/liberation/LiberationSans-Bold.ttf"

LIGHT = np.array([-0.58, -0.66, 0.42]); LIGHT /= np.linalg.norm(LIGHT)   # from the upper left

# Materials: albedo (linear-ish 0..1), metallic 0..1, shininess
MAT = {
    "iron":   ((0.38, 0.33, 0.27), 0.60, 22),
    "brass":  ((0.80, 0.62, 0.33), 0.92, 40),
    "brassd": ((0.58, 0.43, 0.21), 0.85, 30),
    "copper": ((0.74, 0.43, 0.26), 0.90, 36),
    "steel":  ((0.70, 0.71, 0.73), 0.95, 60),
    "dsteel": ((0.20, 0.19, 0.18), 0.80, 50),
    "ivory":  ((0.92, 0.89, 0.81), 0.05, 25),
    "paper":  ((0.86, 0.81, 0.68), 0.0, 8),
    "red":    ((0.66, 0.16, 0.10), 0.3, 30),
    "blued":  ((0.16, 0.25, 0.55), 0.90, 55),   # heat-blued steel, as on real gauge needles
    "glass":  ((0.10, 0.10, 0.10), 0.0, 80),
}

def font(path, size):
    try:
        return ImageFont.truetype(path, int(size))
    except OSError:
        return ImageFont.load_default()

class Scene:
    """Top-most-wins stack of parts on an N x N grid (supersampled)."""
    def __init__(self, n=N):
        self.n = n
        self.h = np.full((n, n), -1e3, np.float32)      # height
        self.nx = np.zeros((n, n), np.float32)
        self.ny = np.zeros((n, n), np.float32)
        self.alb = np.zeros((n, n, 3), np.float32)
        self.met = np.zeros((n, n), np.float32)
        self.shn = np.ones((n, n), np.float32) * 10
        self.mask = np.zeros((n, n), bool)

    def add(self, mask, height, mat, tint=None, detail=None):
        """mask: bool (n,n); height: float array or scalar (top surface)."""
        hgt = np.broadcast_to(np.asarray(height, np.float32), mask.shape)
        put = mask & (hgt >= self.h)
        gy, gx = np.gradient(hgt.astype(np.float32))
        alb, met, shn = MAT[mat]
        a = np.array(alb, np.float32)
        col = np.broadcast_to(a, (self.n, self.n, 3)).copy()
        if tint is not None:
            col *= tint[..., None] if np.ndim(tint) == 2 else tint
        if detail is not None:
            col *= detail[..., None]
        self.h[put] = hgt[put]
        self.nx[put] = -gx[put] * SS     # slope per dial pixel
        self.ny[put] = -gy[put] * SS
        self.alb[put] = col[put]
        self.met[put] = met
        self.shn[put] = shn
        self.mask |= put

# ---------------------------------------------------------------- geometry helpers
YY, XX = np.mgrid[0:N, 0:N].astype(np.float32) / SS      # dial pixels, centre (C, C)

def disc(cx, cy, r):
    return np.hypot(XX - cx, YY - cy) <= r

def ring(cx, cy, r0, r1):
    d = np.hypot(XX - cx, YY - cy)
    return (d >= r0) & (d <= r1)

def poly_mask(pts):
    im = Image.new("L", (N, N), 0)
    ImageDraw.Draw(im).polygon([(x * SS, y * SS) for x, y in pts], fill=255)
    return np.array(im) > 127

def text_mask(text, xy, size, path=SERIF, anchor="mm", angle=0):
    im = Image.new("L", (N, N), 0)
    ImageDraw.Draw(im).text((xy[0] * SS, xy[1] * SS), text, font=font(path, size * SS), fill=255, anchor=anchor)
    return np.array(im, np.float32) / 255

def bevel(mask, base, height, width):
    """Flat top at base+height with edges rounded over `width` dial pixels."""
    d = ndimage.distance_transform_edt(mask) / SS
    t = np.clip(d / max(width, 1e-3), 0, 1)
    prof = np.sin(t * math.pi / 2) ** 0.8
    return base + height * prof

def torus_profile(cx, cy, r0, r1, base, height):
    d = np.hypot(XX - cx, YY - cy)
    t = np.clip((d - r0) / (r1 - r0), 0, 1)
    return base + height * np.clip(np.sin(t * math.pi), 0, 1) ** 0.6

def gear_pts(cx, cy, r_out, r_root, teeth, phase=0.0):
    pts = []
    step = 2 * math.pi / teeth
    for i in range(teeth):
        a0 = phase + step * i
        for f, r in ((0.0, r_root), (0.10, r_root), (0.20, r_out), (0.42, r_out), (0.52, r_root), (0.62, r_root)):
            a = a0 + f * step
            pts.append((cx + r * math.cos(a), cy + r * math.sin(a)))
    return pts

def noise(scale, seed):
    rng = np.random.default_rng(seed)
    s = max(4, int(N / scale))
    small = rng.random((s, s)).astype(np.float32)
    return np.array(Image.fromarray((small * 255).astype(np.uint8)).resize((N, N), Image.BICUBIC), np.float32) / 255

def brushed(cx, cy, seed, amt=0.06):
    r = np.hypot(XX - cx, YY - cy)
    rng = np.random.default_rng(seed)
    return 1 + amt * np.sin(r * 3.1 + 2 * noise(30, seed)) + 0.03 * rng.standard_normal((N, N)).astype(np.float32)

# ---------------------------------------------------------------- parts
def add_gear(S, cx, cy, r_out, teeth, z, thick, mat, phase=0.0, spokes=5, seed=1):
    r_root = r_out - max(4, r_out * 0.10)
    m = poly_mask(gear_pts(cx, cy, r_out, r_root, teeth, phase))
    rim_in = r_root - max(5, r_out * 0.14)
    hub = max(7, r_out * 0.22)
    if spokes and r_out > 22:
        window = ring(cx, cy, hub, rim_in)
        sp = np.zeros_like(m)
        for k in range(spokes):
            a = phase + 2 * math.pi * k / spokes
            ex, ey = cx + rim_in * math.cos(a), cy + rim_in * math.sin(a)
            w = max(3, r_out * 0.07)
            nx_, ny_ = -math.sin(a) * w, math.cos(a) * w
            sp |= poly_mask([(cx + nx_, cy + ny_), (ex + nx_, ey + ny_), (ex - nx_, ey - ny_), (cx - nx_, cy - ny_)])
        m &= ~(window & ~sp)
    h = bevel(m, z, thick, 1.6)
    S.add(m, h, mat, detail=brushed(cx, cy, seed, 0.06) * (0.82 + 0.18 * noise(14, seed + 50)))
    # hub boss and a slotted screw
    hb = disc(cx, cy, hub * 0.8)
    S.add(hb, bevel(hb, z + thick, 2.0, 2.0), mat)
    # a domed bolt on the hub: round, so it looks the same at every angle of the turn
    sc = disc(cx, cy, hub * 0.45)
    dd = np.hypot(XX - cx, YY - cy)
    S.add(sc, z + thick + 2.0 + 1.6 * np.sqrt(np.clip(1 - (dd / (hub * 0.45)) ** 2, 0, 1)), "steel")

def add_rivet(S, x, y, r, z, mat="copper"):
    m = disc(x, y, r)
    d = np.hypot(XX - x, YY - y)
    S.add(m, z + 1.6 * np.sqrt(np.clip(1 - (d / r) ** 2, 0, 1)) * r / 3.0, mat)

def add_gauge(S, cx, cy, r, z, label, value):
    # bezel (rounded ring), face, ticks, label, needle, centre pin
    bz = ring(cx, cy, r - 6, r)
    S.add(bz, torus_profile(cx, cy, r - 6, r, z, 4.0), "brass")
    face = disc(cx, cy, r - 6)
    S.add(face, np.full((N, N), z + 0.5, np.float32), "paper", detail=0.92 + 0.08 * noise(20, 9))
    ink = np.zeros((N, N), np.float32)
    d = ImageDraw.Draw(im := Image.new("L", (N, N), 0))
    for k in range(11):
        a = math.radians(135 + 27 * k)
        r0 = r - 13 if k % 5 else r - 16
        d.line([((cx + r0 * math.cos(a)) * SS, (cy + r0 * math.sin(a)) * SS),
                ((cx + (r - 9) * math.cos(a)) * SS, (cy + (r - 9) * math.sin(a)) * SS)], fill=255, width=SS * (2 if k % 5 == 0 else 1))
    # The danger zone at the top end is HATCHED, not red: red-on-cream is exactly what a
    # red/green colour-blind eye loses, and a hatch reads the same to everybody.
    for k in range(9):
        a = math.radians(135 + 27 * 8 + k * 6.75)
        d.line([((cx + (r - 15) * math.cos(a)) * SS, (cy + (r - 15) * math.sin(a)) * SS),
                ((cx + (r - 9) * math.cos(a + 0.06)) * SS, (cy + (r - 9) * math.sin(a + 0.06)) * SS)], fill=255, width=SS)
    d.arc([(cx - r + 9) * SS, (cy - r + 9) * SS, (cx + r - 9) * SS, (cy + r - 9) * SS], start=135 + 27 * 8, end=135 + 270, fill=255, width=SS)
    ink = np.array(im, np.float32) / 255
    ink = np.maximum(ink, text_mask(label, (cx, cy + r * 0.52), 7, SANS))
    paint = ink > 0.3
    S.alb[paint & face] *= 0.12
    # needle
    a = math.radians(135 + 270 * value)
    ln = r - 12
    nx_, ny_ = -math.sin(a) * 1.4, math.cos(a) * 1.4
    needle = poly_mask([(cx + nx_ * 2, cy + ny_ * 2), (cx + ln * math.cos(a), cy + ln * math.sin(a)), (cx - nx_ * 2, cy - ny_ * 2),
                        (cx - 8 * math.cos(a), cy - 8 * math.sin(a))])
    S.add(needle, np.full((N, N), z + 2.0, np.float32), "blued")
    pin = disc(cx, cy, 3.2)
    S.add(pin, bevel(pin, z + 2.5, 1.5, 1.5), "brass")

# The date windows on the name plate. They lie along a gentle "smile": an arc of radius
# ARC_R centred above them at (ARC_CX, ARC_CY), so the pair bends up at the ends. Each
# window is (screen x of its centre, width along the arc). Shared with clock_style.json so
# the firmware bends its wheels into exactly the frames the plate shows.
PLATE_TOP = C + 57
PLATE_W = 172
WIN_H = 24
ARC_R = 170
ARC_CX = C
ARC_CY = C + 113 - ARC_R          # the windows' middle is lowest at y = C + 113
WIN_MON = (C - 32, 56)
WIN_DAY = (C + 33, 38)
PLATE_RB = ARC_R + WIN_H / 2 + 4 + 7   # the plate's curved bottom edge, 7 px below the frames

def arc_uv(xx, yy, wx):
    """(along, across) coordinates of points relative to the window centred at screen x wx:
    `along` is distance along the arc from the window's centre, `across` is distance from the
    arc's middle line, positive downward (away from the arc's centre)."""
    phc = math.asin((wx - ARC_CX) / ARC_R)
    dx, dy = xx - ARC_CX, yy - ARC_CY
    return (np.arctan2(dx, dy) - phc) * ARC_R, np.hypot(dx, dy) - ARC_R

def round_rect_dist(u, v, hw, hh, rad):
    """Signed distance to a rounded rectangle centred at 0 (negative inside)."""
    qx, qy = np.abs(u) - (hw - rad), np.abs(v) - (hh - rad)
    return np.hypot(np.maximum(qx, 0), np.maximum(qy, 0)) + np.minimum(np.maximum(qx, qy), 0) - rad

def add_plate(S, text, cx, w, z):
    x0, y0, x1 = cx - w / 2, PLATE_TOP, cx + w / 2
    im = Image.new("L", (N, N), 0)
    ImageDraw.Draw(im).rounded_rectangle([x0 * SS, y0 * SS, x1 * SS, (ARC_CY + PLATE_RB) * SS], radius=7 * SS, fill=255)
    m = (np.array(im) > 127) & (np.hypot(XX - ARC_CX, YY - ARC_CY) <= PLATE_RB)
    m = ndimage.binary_opening(m, structure=np.hypot(*np.mgrid[-7 * SS:7 * SS + 1, -7 * SS:7 * SS + 1]) <= 7 * SS)  # round the bottom corners
    S.add(m, bevel(m, z, 2.5, 2.0), "brass", detail=brushed(cx, y0 + 35, 5, 0.03))
    eng = text_mask(text, (cx, y0 + 15), 15)
    e = (eng > 0.25) & m
    S.h[e] -= 1.0
    S.alb[e] *= 0.22
    for rx in (x0 + 9, x1 - 9):
        add_rivet(S, rx, y0 + 15, 3.0, z + 2.5, "brassd")
    # Two curved windows with raised brass frames. The inside is a dark recess; the Orb
    # paints the date wheels into it live, bent to the same curve.
    for (wx, ww) in (WIN_MON, WIN_DAY):
        u, v = arc_uv(XX, YY, wx)
        frame = (round_rect_dist(u, v, ww / 2 + 4, WIN_H / 2 + 4, 4) <= 0) & m
        hole = (np.abs(u) <= ww / 2) & (np.abs(v) <= WIN_H / 2)
        frame &= ~hole
        S.add(frame, bevel(frame, z + 2.5, 1.8, 1.5), "brassd")
        S.h[hole] = z - 1.0
        S.alb[hole] = (0.08, 0.07, 0.06)

def molar_pts(cx, cy, w, h, ang):
    pts = []
    for i in range(31):
        t = i / 30
        pts.append((-w / 2 + w * t, -h * 0.50 + h * 0.07 * (1 - math.cos(t * 2 * math.pi * 1.5)) * 0.9))
    pts += [(w * 0.50, -h * 0.20), (w * 0.44, h * 0.02), (w * 0.36, h * 0.10), (w * 0.40, h * 0.30), (w * 0.30, h * 0.50),
            (w * 0.18, h * 0.50), (w * 0.12, h * 0.22), (0, h * 0.14), (-w * 0.12, h * 0.22), (-w * 0.18, h * 0.50),
            (-w * 0.30, h * 0.50), (-w * 0.40, h * 0.30), (-w * 0.36, h * 0.10), (-w * 0.44, h * 0.02), (-w * 0.50, -h * 0.20)]
    ca, sa = math.cos(ang), math.sin(ang)
    return [(cx + x * ca - y * sa, cy + x * sa + y * ca) for x, y in pts]

# ---------------------------------------------------------------- shading
def shade(S, shadows=True, sky=True):
    h = np.where(S.mask, S.h, S.h[S.mask].min() if S.mask.any() else 0)
    nz = np.ones_like(S.nx)
    nrm = np.stack([S.nx, S.ny, nz], -1)
    nrm /= np.linalg.norm(nrm, axis=-1, keepdims=True)
    ndl = np.clip(nrm @ LIGHT, 0, 1)
    if shadows:
        sh = np.ones_like(h)
        dx, dy = LIGHT[0], LIGHT[1]
        L2 = math.hypot(dx, dy)
        rise = LIGHT[2] / L2              # height gained per pixel toward the light
        for s in range(1, 40 * SS):
            ox, oy = int(round(-dx / L2 * s)), int(round(-dy / L2 * s))
            # sample the height s pixels toward the light (dial pixels -> grid via SS)
            shifted = np.roll(np.roll(h, oy, axis=0), ox, axis=1)
            blocked = shifted > h + rise * s / SS * 1.0 + 0.2
            sh = np.where(blocked, np.minimum(sh, 0.12 + 0.88 * (s / (34.0 * SS)) ** 0.7), sh)
        sh = ndimage.gaussian_filter(sh, 1.2 * SS)
    else:
        sh = np.ones_like(h)
    # ambient occlusion: lower than the neighbourhood -> darker
    blur = ndimage.gaussian_filter(h, 9 * SS)
    ao = np.clip(1 - 0.075 * np.clip(blur - h, 0, 30), 0.25, 1)
    view = np.array([0, 0, 1.0])
    half = LIGHT + view; half /= np.linalg.norm(half)
    spec = np.clip(nrm @ half, 0, 1) ** S.shn
    # fake environment: brighter toward the light side, a little warm
    env = np.clip(0.55 + 0.45 * (nrm[..., :2] @ (-LIGHT[:2] / np.linalg.norm(LIGHT[:2]))), 0, 1) if sky else 0.5
    alb = S.alb
    met = S.met[..., None]
    diffuse = alb * (0.22 * ao[..., None] + 0.95 * ndl[..., None])
    refl = alb * (0.35 + 0.65 * env[..., None]) * ao[..., None]
    col = diffuse * (1 - met * 0.7) + refl * met * 0.7
    col *= (0.30 + 0.70 * sh)[..., None]          # cast shadows darken everything they fall on
    col += (spec * sh)[..., None] * (0.15 + 0.85 * met) * np.where(met > 0.5, alb * 1.3, 0.9)
    return np.nan_to_num(np.clip(col, 0, 1))

# ---------------------------------------------------------------- the dial
def build_dial(name, phase=0.0):
    S = Scene()
    allm = np.ones((N, N), bool)
    # back plate: dark iron with grain and a little rust
    rust = noise(25, 4)
    tint = np.ones((N, N, 3), np.float32)
    tint[..., 0] += 0.5 * np.clip(rust - 0.6, 0, 1); tint[..., 1] += 0.15 * np.clip(rust - 0.6, 0, 1)
    S.add(allm, np.full((N, N), -8, np.float32) + 0.6 * noise(60, 2), "iron", detail=brushed(C, C, 3, 0.10) * (0.75 + 0.25 * noise(12, 41)))
    S.alb *= tint
    for k in range(8):
        a = k * math.pi / 4 + 0.39
        add_rivet(S, C + 128 * math.cos(a), C + 128 * math.sin(a), 3.5, -7.5, "dsteel")

    # mechanism: interlocking gears at different depths (phase turns them for an animation)
    # Spokes repeat every 2 teeth on every gear, so a turn of 2 teeth (one loop) brings each
    # gear back to exactly how it started.
    add_gear(S, C, C, 82, 24, 4, 3.5, "brass", phase, spokes=12, seed=11)
    p2 = -phase * 24 / 14 + math.pi / 14
    add_gear(S, C + 92 * math.cos(-1.05), C + 92 * math.sin(-1.05), 44, 14, 9, 3.0, "copper", p2, spokes=7, seed=12)
    p3 = -phase * 24 / 12
    add_gear(S, C + 90 * math.cos(-2.2), C + 90 * math.sin(-2.2), 38, 12, 2, 3.0, "steel", p3, spokes=6, seed=13)
    p4 = phase * 24 / 10
    add_gear(S, C + 70 * math.cos(1.95) , C + 70 * math.sin(1.95), 22, 10, 10, 2.5, "brassd", p4, spokes=0, seed=14)

    # gauges, mounted above the gears
    # the needles tremble a little through the loop, like a gauge on a running machine
    k = phase / (2 * math.pi / 24 * LOOP_TEETH)                # 0..1 over the loop
    add_gauge(S, C - 92, C + 6, 36, 14, "FLOSS", 0.82 + 0.010 * math.sin(2 * math.pi * k) + 0.004 * math.sin(6 * math.pi * k))
    add_gauge(S, C + 92, C + 6, 36, 14, "SUGAR", 0.12 + 0.008 * math.sin(2 * math.pi * k + 1.3))

    # name plate on two posts
    add_plate(S, name, C, PLATE_W, 15)

    # chapter ring: raised brass with engraved numerals, ivory molars at 12/3/6/9
    rm = ring(C, C, 146, 214)
    S.add(rm, torus_profile(C, C, 146, 214, 18, 3.0), "brassd", detail=brushed(C, C, 21, 0.05) * (0.90 + 0.10 * noise(6, 77)) * 1.18)
    eng = np.zeros((N, N), np.float32)
    romans = ["XII", "I", "II", "III", "IV", "V", "VI", "VII", "VIII", "IX", "X", "XI"]
    for hh in range(12):
        a = hh * math.pi / 6 - math.pi / 2
        x, y = C + 178 * math.cos(a), C + 178 * math.sin(a)
        if hh % 3 == 0:
            mm = poly_mask(molar_pts(x, y, 30, 36, a + math.pi / 2))
            S.add(mm, bevel(mm, 20.5, 2.5, 2.5), "ivory")
        else:
            eng = np.maximum(eng, text_mask(romans[hh], (x, y), 25))
    dd = ImageDraw.Draw(tm := Image.new("L", (N, N), 0))
    for m in range(60):
        a = m * math.pi / 30 - math.pi / 2
        r0 = 200 if m % 5 else 196
        dd.line([((C + r0 * math.cos(a)) * SS, (C + r0 * math.sin(a)) * SS), ((C + 209 * math.cos(a)) * SS, (C + 209 * math.sin(a)) * SS)],
                fill=255, width=SS * (3 if m % 5 == 0 else 1))
    eng = np.maximum(eng, np.array(tm, np.float32) / 255)
    e = (eng > 0.3) & rm
    S.h[e] -= 1.2
    S.alb[e] *= 0.18
    # inner and outer lips of the ring
    for r0, r1 in ((144, 149), (211, 216)):
        lip = ring(C, C, r0, r1)
        S.add(lip, torus_profile(C, C, r0, r1, 19, 2.0), "brassd")

    # copper bezel and its rivets
    bz = ring(C, C, 214, 236)
    S.add(bz, torus_profile(C, C, 214, 236, 22, 6.0), "copper", detail=brushed(C, C, 31, 0.04))
    for k in range(24):
        a = k * math.pi / 12 + math.pi / 24
        add_rivet(S, C + 225 * math.cos(a), C + 225 * math.sin(a), 3.2, 27, "brassd")
    return S

def finish(col):
    im = Image.fromarray((col ** (1 / 1.15) * 255).astype(np.uint8), "RGB").resize((W, W), Image.LANCZOS)
    a = np.array(im, np.float32)
    yy, xx = np.mgrid[0:W, 0:W]
    rr = np.hypot(xx - C + 0.5, yy - C + 0.5)
    a[rr > 233.5] = 0
    return Image.fromarray(a.astype(np.uint8), "RGB")

# ---------------------------------------------------------------- hands
def hand_scene(w, h, draw):
    """Render a hand in its own small height field. draw(S, X, Y) adds the parts."""
    global XX, YY, N
    saveX, saveY, saveN = XX, YY, N
    N = max(w, h) * SS
    YY, XX = np.mgrid[0:N, 0:N].astype(np.float32) / SS
    S = Scene(N)
    draw(S)
    col = shade(S, shadows=False)
    # A thin dark outline, so polished hands read against the brass ring and the dark
    # mechanism alike.
    ring_px = ndimage.binary_dilation(S.mask, iterations=int(1.6 * SS)) & ~S.mask
    col[ring_px] = (0.06, 0.05, 0.04)
    alpha = (S.mask | ring_px).astype(np.float32)
    XX, YY, N = saveX, saveY, saveN
    rgba = np.dstack([col ** (1 / 1.15), alpha])
    im = Image.fromarray((rgba * 255).astype(np.uint8), "RGBA").resize((max(w, h), max(w, h)), Image.LANCZOS)
    return im.crop((0, 0, w, h))

def hour_hand():
    w, h, px, py = 64, 172, 32, 142
    def draw(S):
        stem = poly_mask([(px - 5, py + 26), (px + 5, py + 26), (px + 6, py), (px + 4, 66), (px - 4, 66), (px - 6, py)])
        S.add(stem, bevel(stem, 0, 2.5, 2.0), "steel")
        eye = ring(px, 83, 8, 13)
        S.add(eye, torus_profile(px, 83, 8, 13, 0, 2.5), "steel")
        right = [(4, 12), (9, 20), (13, 28), (14.5, 34), (13.5, 40), (10, 45), (6, 48), (3, 52), (2.5, 66)]
        sp = poly_mask([(px, 3)] + [(px + dx, y) for dx, y in right] + [(px - dx, y) for dx, y in reversed(right)])
        sp &= ~disc(px, 34.5, 4.5)
        S.add(sp, bevel(sp, 0, 3.0, 3.0), "steel")
        boss = disc(px, py, 14)
        S.add(boss, bevel(boss, 0.5, 3.0, 3.0), "steel")
        tail = disc(px, py + 28, 8)
        S.add(tail, bevel(tail, 0, 2.5, 2.5), "steel")
    return hour_hand_geom(hand_scene(w, h, draw), px, py)

def hour_hand_geom(im, px, py):
    return im, px, py

def minute_hand():
    w, h, px, py = 48, 222, 24, 196
    def draw(S):
        stem = poly_mask([(px - 4, py + 28), (px + 4, py + 28), (px + 5, py), (px + 2.5, 40), (px - 2.5, 40), (px - 5, py)])
        S.add(stem, bevel(stem, 0, 2.2, 1.8), "steel")
        eye = ring(px, 114, 6, 10)
        S.add(eye, torus_profile(px, 114, 6, 10, 0, 2.2), "steel")
        tail = disc(px, py + 29, 7)
        S.add(tail, bevel(tail, 0, 2.2, 2.2), "steel")
        boss = disc(px, py, 12)
        S.add(boss, bevel(boss, 0.5, 2.8, 2.8), "steel")
        neck = poly_mask([(px - 2, 44), (px + 2, 44), (px + 2, 30), (px - 2, 30)])
        S.add(neck, bevel(neck, 0, 2.0, 1.0), "steel")
        rim = disc(px, 18, 16)
        S.add(rim, torus_profile(px, 18, 11, 16, 1.0, 2.5) * (np.hypot(XX - px, YY - 18) >= 11) + 1.0, "steel")
        mir = disc(px, 18, 11.5)
        d = np.hypot(XX - px, YY - 18)
        S.add(mir, 1.5 + 1.2 * np.sqrt(np.clip(1 - (d / 11.5) ** 2, 0, 1)), "steel")
        S.alb[mir] = np.array([0.80, 0.86, 0.92]) * (0.85 + 0.25 * np.clip(((px - XX[mir]) + (18 - YY[mir])) / 16, -1, 1))[:, None]
    return hand_scene(w, h, draw), px, py

def second_hand():
    w, h, px, py = 34, 252, 17, 206
    def draw(S):
        nd = poly_mask([(px - 1.6, py), (px + 1.6, py), (px + 0.8, 20), (px - 0.8, 20)])
        S.add(nd, bevel(nd, 0, 1.2, 0.8), "steel")
        im = Image.new("L", (N, N), 0)
        ImageDraw.Draw(im).arc([(px - 1) * SS, 8 * SS, (px + 11) * SS, 24 * SS], start=180, end=330, fill=255, width=int(1.8 * SS))
        hook = np.array(im) > 127
        S.add(hook, np.full((N, N), 1.0, np.float32), "steel")
        arm = poly_mask([(px - 1.5, py), (px + 1.5, py), (px + 1.5, py + 24), (px - 1.5, py + 24)])
        S.add(arm, bevel(arm, 0, 1.2, 0.8), "steel")
        g = poly_mask(gear_pts(px, py + 30, 10, 8, 10)) & ~disc(px, py + 30, 3.2)
        S.add(g, bevel(g, 0, 1.8, 1.2), "copper")
        cap = poly_mask(gear_pts(px, py, 13, 11, 12))
        S.add(cap, bevel(cap, 1.0, 2.5, 2.0), "brass")
        dome = disc(px, py, 7)
        dd = np.hypot(XX - px, YY - py)
        S.add(dome, 3.5 + 2.5 * np.sqrt(np.clip(1 - (dd / 7) ** 2, 0, 1)), "brass")
        pin = disc(px, py, 2.4)
        S.add(pin, np.full((N, N), 6.5, np.float32), "dsteel")
    return hand_scene(w, h, draw), px, py

def shadow_of(hand):
    a = hand.split()[3].filter(ImageFilter.GaussianBlur(3))
    a = a.point(lambda v: int(v * 0.6))
    sh = Image.new("RGBA", hand.size, (0, 0, 0, 255))
    sh.putalpha(a)
    return sh

# ---------------------------------------------------------------- glass
def glass():
    yy, xx = np.mgrid[0:W, 0:W].astype(np.float32)
    r = np.hypot(xx - C, yy - C) / 216.0
    a = np.zeros((W, W), np.float32)
    rgb = np.ones((W, W, 3), np.float32)
    # a broad soft glare across the upper left, like a window caught in the dome
    u = ((xx - C) * 0.70 + (yy - C) * 0.71) / 216.0
    band = np.exp(-((u + 0.45) / 0.16) ** 2) * np.clip(1 - r, 0, 1) ** 0.5
    a += 0.20 * band
    # a crisp thin reflection arc near the rim
    th = np.arctan2(yy - C, xx - C)
    arc = np.exp(-((r - 0.93) / 0.012) ** 2) * np.exp(-((th + 2.2) / 0.45) ** 2)
    a += 0.45 * arc
    # the dome darkens toward its edge
    edge = np.clip((r - 0.70) / 0.30, 0, 1) ** 2
    a = np.clip(a, 0, 1)
    dark = 0.28 * edge
    out_rgb = rgb * (a / np.maximum(a + dark, 1e-6))[..., None]
    out_a = np.clip(a + dark, 0, 1)
    # The cyclops: a small magnifier set in the crystal over the day window, as on a Rolex.
    # It cannot actually magnify (the glass is drawn over the dial, not through it), so it
    # is a lens rim and a glint that sit exactly over the window.
    # It follows the window's curve: measured along and across the arc, not in x and y.
    lu, lv = arc_uv(xx, yy, WIN_DAY[0])
    lw, lh = WIN_DAY[1] + 14, WIN_H + 14
    dist = np.maximum(round_rect_dist(lu, lv, lw / 2, lh / 2, 7) + 7, 0)   # 0 inside, as before
    rim = np.exp(-((dist - 7) / 1.1) ** 2)
    glint = np.exp(-(((lu + lw * 0.22) / 5) ** 2 + ((lv + lh * 0.28) / 2.2) ** 2))
    lens_a = 0.35 * rim + 0.55 * glint
    out_rgb = (out_rgb * out_a[..., None] + lens_a[..., None]) / np.maximum(out_a + lens_a, 1e-6)[..., None]
    out_a = np.clip(out_a + lens_a, 0, 1)
    out_a[r > 1.08] = 0
    return Image.fromarray(np.dstack([out_rgb * 255, out_a * 255]).astype(np.uint8), "RGBA")

# ---------------------------------------------------------------- theme
def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("outdir")
    ap.add_argument("--name", default="BATEMAN DENTAL")
    ap.add_argument("--frames", type=int, default=12,
                    help="dial pictures in the turning-gear loop (1 = a still dial)")
    ap.add_argument("--fps", type=int, default=4)
    a = ap.parse_args()
    d = os.path.join(a.outdir, SLUG)
    os.makedirs(d, exist_ok=True)

    # The turning gears: over one loop every gear advances exactly LOOP_TEETH of its own
    # teeth, and its spokes repeat every LOOP_TEETH teeth, so the last picture flows straight
    # back into the first. Frame 0 is clock_plate.png; the rest are clock_plate_01.png ...
    frames = max(1, a.frames)
    for f in range(frames):
        phase = 2 * math.pi / 24 * LOOP_TEETH * f / frames
        plate = finish(shade(build_dial(a.name, phase)))
        # RGBA: the firmware's PNG loader reads RGBA pixels only (RGB decodes to black).
        name = "clock_plate.png" if f == 0 else f"clock_plate_{f:02d}.png"
        plate.convert("RGBA").save(os.path.join(d, name), optimize=True)
        print(f"  {name}")

    glass().save(os.path.join(d, "clock_overlay.png"), optimize=True)
    hands = {}
    for key, fn in (("hour", hour_hand), ("minute", minute_hand), ("second", second_hand)):
        im, px, py = fn()
        im.save(os.path.join(d, f"clock_hand_{key}.png"), optimize=True)
        hands[key] = {"show": True, "pivotX": px, "pivotY": py, "centerX": 233, "centerY": 233}
        if key != "second":
            shadow_of(im).save(os.path.join(d, f"clock_shadow_{key}.png"), optimize=True)
    hands["static1"] = {"show": False}
    hands["static2"] = {"show": False}
    hands["order"] = [0, 1, 2]
    hands["shadow"] = {"on": True, "dx": 4, "dy": 6}
    style = {"bg": 0, "hands": hands, "text1": {"show": False}, "text2": {"show": False}, "secondSweep": True}
    # The Rolex-style date: month and day wheels the firmware draws live into the plate's
    # two windows (theme_style::Clock::DateWheel), under the hands, rolling at midnight.
    style["dateWheel"] = {"show": True,
                          "monX": int(WIN_MON[0]), "monY": int(ARC_CY + ARC_R), "monW": WIN_MON[1],
                          "dayX": int(WIN_DAY[0]), "dayY": int(ARC_CY + ARC_R), "dayW": WIN_DAY[1],
                          "h": WIN_H, "arcCX": int(ARC_CX), "arcCY": int(ARC_CY), "arcR": ARC_R,
                          "wheel": 0xEDE4CC, "ink": 0x1C1814}
    if frames > 1:
        # Zion's moving background: EXTRA frames beyond the plate, looping for ever.
        style["bgAnim"] = {"frames": frames - 1, "fps": a.fps, "loop": True}
    with open(os.path.join(d, "clock_style.json"), "w") as f:
        json.dump(style, f, indent=2)
    assets = sorted(n for n in os.listdir(d) if n.endswith(".png"))
    with open(os.path.join(d, "theme.json"), "w") as f:
        json.dump({"name": "Bateman Steampunk", "author": "Kevin's Orb", "assets": assets}, f, indent=2)
    open(os.path.join(d, "_installed"), "w").close()
    print(f"wrote {d}: {', '.join(assets)}")

if __name__ == "__main__":
    main()

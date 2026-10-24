#!/usr/bin/env python3
"""Kevin's Orb - synthesize the warp whoosh.

Writes the sound the warp transition plays (src/warp.cpp), timed to its animation:
  0.00-0.44 s  rising rush of filtered noise and a climbing low hum (the jump)
  0.44 s       a bright shimmer at the moment the new screen arrives
  0.44-1.00 s  the rush falls away

Outputs (both 16 kHz, 16-bit, stereo):
  OUTDIR/warp.pcm   raw PCM, the format the Orb's audio.cpp streams from the SD card
  OUTDIR/warp.wav   the same sound with a WAV header, to listen to on a computer

Original synthesis; no samples. Usage: python3 make_warp.py OUTDIR   (needs numpy)
"""
import os, struct, sys, wave
import numpy as np

SR = 16000
DUR = 1.0
T_SWAP = 0.44

def bandpass_sweep(noise, f_lo, f_hi, q=4.0):
    """Two-pole resonant band-pass whose centre frequency follows the arrays f_lo..f_hi."""
    out = np.zeros_like(noise)
    y1 = y2 = 0.0
    for i, x in enumerate(noise):
        f = f_lo[i]
        w = 2 * np.pi * f / SR
        r = np.exp(-w / (2 * q))
        a1, a2 = 2 * r * np.cos(w), -r * r
        y = (1 - r) * x + a1 * y1 + a2 * y2
        out[i] = y
        y2, y1 = y1, y
    return out

def main(outdir):
    n = int(SR * DUR)
    t = np.arange(n) / SR
    rng = np.random.default_rng(7)

    # Centre frequency of the rush: up during the jump, back down after the arrival.
    up = np.clip(t / T_SWAP, 0, 1)
    down = np.clip((t - T_SWAP) / (DUR - T_SWAP), 0, 1)
    fc = np.where(t < T_SWAP, 250 + 2600 * up ** 2, 2850 - 2300 * down ** 0.7)

    env = np.where(t < T_SWAP, up ** 1.6, (1 - down) ** 2.2)
    rush_l = bandpass_sweep(rng.standard_normal(n), fc, None) * env
    rush_r = bandpass_sweep(rng.standard_normal(n), fc * 1.04, None) * env

    hum_f = np.where(t < T_SWAP, 55 + 70 * up ** 2, 125 - 60 * down)
    hum = np.sin(2 * np.pi * np.cumsum(hum_f) / SR) * env * 0.35

    # Arrival shimmer: a few bright partials that ring briefly.
    sh_t = np.clip(t - T_SWAP, 0, None)
    sh_env = np.where(t >= T_SWAP, np.exp(-sh_t * 9.0), 0.0)
    shimmer = sum(np.sin(2 * np.pi * f * t) for f in (1760, 2637, 3520)) * sh_env * 0.12

    # A little left-right movement so it feels like it passes through.
    pan = 0.5 + 0.35 * np.sin(2 * np.pi * 1.3 * t)
    left = rush_l * (1.2 - pan) + hum + shimmer
    right = rush_r * (0.2 + pan) + hum + shimmer

    # Soft edges and level: peak about -4 dBFS.
    fade = np.minimum(1, np.minimum(t / 0.01, (DUR - t) / 0.03))
    st = np.stack([left, right], axis=1) * fade[:, None]
    st *= 0.63 / np.max(np.abs(st))
    pcm = (st * 32767).astype("<i2")

    os.makedirs(outdir, exist_ok=True)
    pcm.tofile(os.path.join(outdir, "warp.pcm"))
    with wave.open(os.path.join(outdir, "warp.wav"), "wb") as w:
        w.setnchannels(2); w.setsampwidth(2); w.setframerate(SR)
        w.writeframes(pcm.tobytes())
    print(f"warp.pcm {pcm.nbytes} bytes, {DUR:.2f} s")

if __name__ == "__main__":
    main(sys.argv[1] if len(sys.argv) > 1 else ".")

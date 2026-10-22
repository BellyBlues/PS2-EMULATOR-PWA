#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
# SPDX-License-Identifier: GPL-3.0+

"""Takes the trademarks out of the disc library's console photo (pcsx2-wasm/web/library/ps2.webp, after
tools/wasm/cutout-photo.py): the PS2 logo and script on the top, the small logos along its front
edge, the PlayStation logo on the tray, "SONY" beside the buttons and "MagicGate". Each is filled
from its surroundings (the top's gradient and grain, the ridges beside it).

Usage:
    python3 tools/wasm/delogo-photo.py CUTOUT.webp OUTPUT.webp   (2200 or 4400 px across)

Needs numpy, opencv-python and Pillow (with WebP).
"""

import sys
import numpy as np
import cv2
from PIL import Image

src, dst = sys.argv[1], sys.argv[2]
im = np.array(Image.open(src).convert("RGBA"))
rgb = im[:, :, :3].astype(np.float32)
alpha = im[:, :, 3]
S = im.shape[1] // 1100  # page units -> pixels (the page's photo is 2200 px, the original 4400)

def box(x0, y0, x1, y1):
    return slice(y0 * S, y1 * S), slice(x0 * S, x1 * S)

def feather(mask, r):
    m = cv2.GaussianBlur(mask.astype(np.float32), (0, 0), r)
    return np.clip(m, 0, 1)[..., None]

# 1. The top surface: PS2 logo, "PlayStation 2" script, and the row of small logos at its front edge.
#    The surface is a soft gradient with a fine texture: fill = smooth inpaint + texture from a clean patch.
ref = box(300, 150, 450, 250)
ref_patch = rgb[ref]
GRAIN = float((ref_patch - cv2.GaussianBlur(ref_patch, (0, 0), 6)).mean(axis=2).std())


def surface_fill(region, poly_pts, clean_shift=None):
    ys, xs = region
    sub = rgb[ys, xs].copy()
    mask = np.zeros(sub.shape[:2], np.uint8)
    pts = (np.array(poly_pts, np.float32) * S - [xs.start, ys.start]).astype(np.int32)
    cv2.fillPoly(mask, [pts], 255)
    low = cv2.GaussianBlur(sub, (0, 0), 6)
    lowfill = cv2.inpaint(np.clip(low, 0, 255).astype(np.uint8), mask, 12, cv2.INPAINT_TELEA).astype(np.float32)
    lowfill = cv2.GaussianBlur(lowfill, (0, 0), 10)
    # The grain: noise with the spread of a clean part of the top (behind the logo, left of it).
    rng = np.random.default_rng(7)
    noise = rng.normal(0, 1, sub.shape[:2]).astype(np.float32)
    noise = cv2.GaussianBlur(noise, (0, 0), 0.9)
    noise *= 0.6 * GRAIN / max(noise.std(), 1e-6)
    fill = lowfill + noise[..., None]
    f = feather(mask > 0, 4)
    rgb[ys, xs] = sub * (1 - f) + fill * f

# The big logo and the script (a polygon around them on the top face).
surface_fill(box(470, 160, 740, 370),
             [(540, 172), (722, 172), (720, 242), (632, 350), (508, 372), (478, 300), (496, 222)],
             (-260 * S, 10 * S))
# The small logos along the front edge of the top.
surface_fill(box(590, 380, 800, 440),
             [(605, 389), (790, 412), (785, 433), (600, 405)],
             (0, -55 * S))

# 2. The PlayStation logo on the tray's front: ridges copied from beside it, along the ridges' slope.
def ridge_copy(x0, y0, x1, y1, dx, slope, feather_r=3):
    ys, xs = box(x0, y0, x1, y1)
    sdx = dx * S
    sdy = int(round(dx * slope * S))
    src_patch = rgb[ys.start + sdy:ys.stop + sdy, xs.start + sdx:xs.stop + sdx]
    mask = np.zeros(src_patch.shape[:2], np.uint8)
    h, w = mask.shape
    cv2.rectangle(mask, (8, 8), (w - 9, h - 9), 255, -1)
    f = feather(mask > 0, feather_r * S)
    rgb[ys, xs] = rgb[ys, xs] * (1 - f) + src_patch * f

ridge_copy(588, 440, 632, 480, -60, 0.26)

# 3. "SONY" on the strip right of the buttons: the letters (light pixels) painted from around them.
def paint_out_letters(x0, y0, x1, y1, level):
    ys, xs = box(x0, y0, x1, y1)
    sub = np.clip(rgb[ys, xs], 0, 255).astype(np.uint8)
    mask = (sub.mean(axis=2) > level).astype(np.uint8) * 255
    mask = cv2.dilate(mask, np.ones((5, 5), np.uint8), iterations=2)
    rgb[ys, xs] = cv2.inpaint(sub, mask, 6, cv2.INPAINT_TELEA).astype(np.float32)

paint_out_letters(814, 474, 838, 550, 70)

# 4. "MagicGate" next to memory card slot 1.
def shift_copy(x0, y0, x1, y1, dx, dy, feather_r=2):
    ys, xs = box(x0, y0, x1, y1)
    src_patch = rgb[ys.start + dy * S:ys.stop + dy * S, xs.start + dx * S:xs.stop + dx * S].copy()
    mask = np.zeros(src_patch.shape[:2], np.uint8)
    h, w = mask.shape
    cv2.rectangle(mask, (6, 6), (w - 7, h - 7), 255, -1)
    f = feather(mask > 0, feather_r * S)
    rgb[ys, xs] = rgb[ys, xs] * (1 - f) + src_patch * f

# The ridge band continued from a clean stretch right of the text (x 279-285), along the ridges' slope.
def extrude_band(x0, y0, x1, y1, xr0, xr1, slope, feather_r=2):
    ys, xs = box(x0, y0, x1, y1)
    h, w = ys.stop - ys.start, xs.stop - xs.start
    ref = rgb[:, xr0 * S:xr1 * S].mean(axis=1)  # one column, averaged over the clean stretch
    xc = (xr0 + xr1) / 2 * S
    out = np.zeros((h, w, 3), np.float32)
    for j in range(w):
        x = xs.start + j
        for i in range(h):
            yy = ys.start + i + slope * (xc - x)
            y0f = int(np.floor(yy))
            t = yy - y0f
            out[i, j] = ref[y0f] * (1 - t) + ref[y0f + 1] * t
    mask = np.zeros((h, w), np.uint8)
    cv2.rectangle(mask, (5, 5), (w - 6, h - 6), 255, -1)
    f = feather(mask > 0, feather_r * S)
    rgb[ys, xs] = rgb[ys, xs] * (1 - f) + out * f

extrude_band(216, 330, 277, 351, 279, 285, 0.26)

out = np.dstack([np.clip(rgb, 0, 255).astype(np.uint8), alpha])
Image.fromarray(out, "RGBA").save(dst, quality=88, method=6)

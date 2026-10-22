#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
# SPDX-License-Identifier: GPL-3.0+

"""Makes the photos of the page's bottom bar (UI-SPEC.md §16): the PS2's front, straightened, and a
memory card, both from Evan-Amos's public-domain photos, with their Sony and PlayStation marks taken out.

    python3 tools/wasm/console-bar-photos.py CONSOLE.jpg CARD.jpg OUTDIR

CONSOLE.jpg: https://commons.wikimedia.org/wiki/File:Sony-PlayStation-2-30001-Console-FL.jpg (4400 px)
CARD.jpg:    https://commons.wikimedia.org/wiki/File:PS2-8MB-Mem-Card.jpg (2300 px)
Writes OUTDIR/ps2-front.webp and OUTDIR/memory-card.webp. The top of the console comes from the photo
cleaned by tools/wasm/delogo-photo.py (run at the photo's own size). Needs numpy, opencv-python and
Pillow (with WebP).
"""

import os
import subprocess
import sys
import tempfile

import cv2
import numpy as np
from PIL import Image

HERE = os.path.dirname(os.path.abspath(__file__))


def straighten(im, quad, w, h):
    m = cv2.getPerspectiveTransform(np.float32(quad), np.float32([(0, 0), (w, 0), (w, h), (0, h)]))
    return cv2.warpPerspective(im, m, (w, h), flags=cv2.INTER_CUBIC, borderMode=cv2.BORDER_REPLICATE)


def feather(h, w, m, r):
    mask = np.zeros((h, w), np.float32)
    mask[m:h - m, m:w - m] = 1
    return cv2.GaussianBlur(mask, (0, 0), r)[..., None]


def console_front(console_jpg, out):
    orig = np.array(Image.open(console_jpg).convert('RGB'))
    with tempfile.TemporaryDirectory() as tmp:
        rgba = os.path.join(tmp, 'in.png')
        Image.fromarray(orig).convert('RGBA').save(rgba)
        cleaned = os.path.join(tmp, 'clean.png')
        subprocess.run([sys.executable, os.path.join(HERE, 'delogo-photo.py'), rgba, cleaned], check=True)
        clean = np.array(Image.open(cleaned).convert('RGB'))
    s = orig.shape[1] / 1100.0  # the corners are in the 1100-unit space the page's photo uses
    W, H_TOP, H_FRONT = 2400, 108, 324
    top_quad = np.float32([(124, 241.5), (870.6, 420.3), (836, 461), (84, 280)]) * s
    front_quad = np.float32([(84, 280), (836, 461), (836, 560), (88, 387)]) * s
    img = np.vstack([straighten(clean, top_quad, W, H_TOP), straighten(orig, front_quad, W, H_FRONT)]).astype(np.float32)

    def copy_rows(x0, x1, y0, y1, dx, dy=0):
        f = feather(y1 - y0, x1 - x0, 6, 4)
        img[y0:y1, x0:x1] = img[y0:y1, x0:x1] * (1 - f) + img[y0 + dy:y1 + dy, x0 + dx:x1 + dx] * f

    def extrude(x0, x1, y0, y1, cx0, cx1):
        fill = np.repeat(img[y0:y1, cx0:cx1].mean(axis=1, keepdims=True), x1 - x0, axis=1)
        fill += np.random.default_rng(3).normal(0, 1.2, fill.shape[:2])[..., None]
        f = feather(y1 - y0, x1 - x0, 4, 3)
        img[y0:y1, x0:x1] = img[y0:y1, x0:x1] * (1 - f) + fill * f

    extrude(398, 562, 156, 192, 566, 600)       # "MagicGate", from the ridge right of it
    copy_rows(1580, 1712, 228, 334, -150, -2)   # the PlayStation logo on the tray (the ridges drop 2 px over 150)
    # Below the panel on the left is the recessed part with the USB ports: shade it to the panel's shadow.
    y0, y1 = 386, 414
    fade = np.clip((np.arange(y0, y1) - y0) / (y1 - y0 - 1), 0, 1)[:, None, None]
    img[y0:y1, :1830] = img[y0:y1, :1830] * (1 - fade) + 9.0 * fade
    # Most of the top band off, and "SONY" (on the corner past the buttons), the USB ports and the
    # studio's background at the left edge cut away.
    res = np.clip(img, 0, 255).astype(np.uint8)[48:414, 8:2318]
    Image.fromarray(res).save(os.path.join(out, 'ps2-front.webp'), quality=88, method=6)


def memory_card(card_jpg, out):
    im = np.array(Image.open(card_jpg).convert('RGB'))
    s = im.shape[1] / 1150.0
    quad = np.float32([(650, 42), (1105, 325), (505, 795), (42, 455)]) * s  # its face; the slot end first
    card = straighten(im, quad, 420, 560)
    H, W = card.shape[:2]
    mask = np.zeros((H, W), np.uint8)
    r, b = card[:, :, 0].astype(int), card[:, :, 2].astype(int)
    mask[(b > r + 25) & (b > 90)] = 255                     # the blue PS logo and "PlayStation 2"
    lum = card.mean(axis=2)
    box = np.zeros_like(mask)
    box[236:266, 112:296] = 1                               # "MagicGate"
    mask[(box == 1) & (lum > 62)] = 255
    mask[488:538, 104:300] = 255                            # "SONY", embossed: the whole patch
    mask = cv2.dilate(mask, np.ones((5, 5), np.uint8), iterations=2)
    clean = cv2.inpaint(card, mask, 9, cv2.INPAINT_TELEA).astype(np.float32)
    noise = cv2.GaussianBlur(np.random.default_rng(5).normal(0, 1, (H, W)).astype(np.float32), (0, 0), 0.8)
    noise *= 2.2 / noise.std()
    f = cv2.GaussianBlur((mask > 0).astype(np.float32), (0, 0), 2)[..., None]
    clean = np.clip(clean + noise[..., None] * f, 0, 255).astype(np.uint8)
    alpha = np.where(lum > 175, 0, 255).astype(np.uint8)    # the studio's white around it
    corner = np.full((H, W), 255, np.uint8)
    rad = 18
    corner[:rad, :rad] = corner[:rad, -rad:] = corner[-rad:, :rad] = corner[-rad:, -rad:] = 0
    for cx, cy in [(rad, rad), (W - 1 - rad, rad), (rad, H - 1 - rad), (W - 1 - rad, H - 1 - rad)]:
        cv2.circle(corner, (cx, cy), rad, 255, -1)
    alpha = cv2.GaussianBlur(np.minimum(alpha, corner).astype(np.float32), (0, 0), 0.8).astype(np.uint8)
    Image.fromarray(np.dstack([clean, alpha]), 'RGBA').save(os.path.join(out, 'memory-card.webp'), quality=90, method=6)


if __name__ == '__main__':
    if len(sys.argv) != 4:
        sys.exit(__doc__)
    console_front(sys.argv[1], sys.argv[3])
    memory_card(sys.argv[2], sys.argv[3])

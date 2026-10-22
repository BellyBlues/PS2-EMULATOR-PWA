#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
# SPDX-License-Identifier: GPL-3.0+

"""Cuts a product photo out of its white studio background, for the disc library's console photo
(pcsx2-wasm/web/library/ps2.webp, see library/ps2-photo.txt).

Usage:
    python3 tools/wasm/cutout-photo.py SOURCE.jpg OUTPUT.webp WIDTH

Needs numpy, scipy and Pillow (with WebP).
"""

# Removes the white studio background of a product photo: the background is the near-white region
# connected to the image border; inside it, darker pixels (the soft shadow) keep a partial alpha.
import sys
import numpy as np
from PIL import Image
from scipy import ndimage

src, dst, width = sys.argv[1], sys.argv[2], int(sys.argv[3])
im = Image.open(src).convert("RGB")
im = im.resize((width, round(im.height * width / im.width)), Image.LANCZOS)
a = np.asarray(im).astype(np.float32)
lum = a.mean(axis=2)
light = lum > 150
labels, _ = ndimage.label(light)
border = set(np.unique(np.concatenate([labels[0], labels[-1], labels[:, 0], labels[:, -1]]))) - {0}
bg = np.isin(labels, list(border))
# Grow the background a little into the anti-aliased edge.
alpha = np.ones(lum.shape, np.float32)
# Shadow: darkness of the background region becomes a soft black.
shadow = np.clip((245 - lum) / 120, 0, 1) * 0.55
alpha[bg] = shadow[bg]
rgb = a.copy()
rgb[bg] = 0
# Edge pixels between console and white: alpha from luminance to avoid a white fringe.
edge = ndimage.binary_dilation(bg, iterations=2) & ~bg
alpha[edge] = np.clip((255 - lum[edge]) / 200, 0, 1)
rgb[edge] = np.clip(a[edge] - (1 - alpha[edge, None]) * 255, 0, 255) / np.maximum(alpha[edge, None], 1e-3)
out = np.dstack([np.clip(rgb, 0, 255), alpha * 255]).astype(np.uint8)
Image.fromarray(out, "RGBA").save(dst, quality=88, method=6)
print(out.shape)

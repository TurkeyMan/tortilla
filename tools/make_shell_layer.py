#!/usr/bin/env python3
# Makes the shell layer of Tortilla's icon from the master PNG: the taco
# shell's shading in grey (its usual tone at 200/255), its alpha the shell's
# mask. make_icons splits each icon size into the shell and the rest, and
# Tortilla multiplies the shell by a repository's colour, so windows of
# different repositories tell apart on the taskbar.
# Needs Pillow, NumPy and SciPy; it isn't part of the build:
#   python3 tools/make_shell_layer.py data/icons/tortilla-master.png data/icons/tortilla-shell-master.png
import sys

import numpy as np
from PIL import Image
from scipy import ndimage as nd

src, out = sys.argv[1], sys.argv[2]
im = np.asarray(Image.open(src).convert('RGBA')).astype(np.float32) / 255
rgb, a = im[..., :3], im[..., 3]
r, g, b = rgb[..., 0], rgb[..., 1], rgb[..., 2]
mx, mn = rgb.max(-1), rgb.min(-1)
v = mx
s = np.where(mx > 0, (mx - mn) / np.maximum(mx, 1e-6), 0)
d = np.maximum(mx - mn, 1e-6)
h = np.where(mx == r, ((g - b) / d) % 6, np.where(mx == g, (b - r) / d + 2, (r - g) / d + 4)) * 60

# The tan body, and its orange rim (more saturated, redder than the cheese).
body = (((h > 22) & (h < 48) & (s > 0.25) & (s < 0.78)) | ((h > 24) & (h < 41) & (s >= 0.78)))
body &= (v > 0.45) & (a > 0.5)
body = nd.binary_opening(body, iterations=2)
# The dome is the largest piece. The strip under the head is the turtle's
# neck and belly, the same tan, so it's left out.
lab, n = nd.label(body)
dome = nd.binary_fill_holes(lab == 1 + np.argmax(nd.sum(body, lab, range(1, n + 1))))
# The cream margin inside the outline: pale yellow, reached from the dome
# without crossing the dark outline (and only so far, so nothing leaks).
cream = (h > 25) & (h < 62) & (v > 0.55) & (a > 0.5)
for _ in range(45):
    grown = nd.binary_dilation(dome) & cream
    if not (grown & ~dome).any():
        break
    dome |= grown
# The outline's soft inner edge, which tints to near-black anyway, so no
# untinted fringe is left between the two.
dome |= nd.binary_dilation(dome, iterations=3) & (v <= 0.55) & (a > 0.5)
mask = nd.binary_fill_holes(dome)

L = 0.299 * r + 0.587 * g + 0.114 * b
grey = np.clip(L / np.median(L[mask]) * 200, 0, 255).astype(np.uint8)
Image.fromarray(np.dstack([grey, grey, grey, (mask * 255).astype(np.uint8)]), 'RGBA').save(out)
print(f'{out}: {int(mask.sum())} shell pixels')

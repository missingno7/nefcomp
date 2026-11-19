"""Render a raw uint16 RGGB dump (or a crop of it) to a PNG for eyeballing.

usage: python preview.py in.u16 out.png [W H] [--crop x y w h] [--black 1008]
"""
import sys
import numpy as np
from PIL import Image

args = sys.argv[1:]
crop = None
black = 1008.0
if '--crop' in args:
    i = args.index('--crop'); crop = list(map(int, args[i + 1:i + 5])); del args[i:i + 5]
if '--black' in args:
    i = args.index('--black'); black = float(args[i + 1]); del args[i:i + 2]
src, dst = args[0], args[1]
W, H = (int(args[2]), int(args[3])) if len(args) > 3 else (8280, 5520)
a = np.fromfile(src, dtype='<u2').reshape(H, W).astype(np.float32)
if crop:
    x, y, w, h = crop
    a = a[y & ~1:(y & ~1) + h, x & ~1:(x & ~1) + w]
r, g1, g2, b = a[0::2, 0::2], a[0::2, 1::2], a[1::2, 0::2], a[1::2, 1::2]
rgb = np.stack([r - black, (g1 + g2) / 2 - black, b - black], -1)
rgb *= np.array([2.0, 1.0, 1.6], dtype=np.float32)  # rough daylight WB
scale = np.percentile(rgb, 99.5)
rgb = np.clip(rgb / scale, 0, 1) ** (1 / 2.2)
img = Image.fromarray((rgb * 255).astype(np.uint8))
if not crop:
    img = img.resize((img.width // 4, img.height // 4), Image.LANCZOS)
img.save(dst)

"""Contact sheet of 100% crops for several quality levels (bilinear demosaic, simple WB/gamma).

usage: python levelcrops.py dir W H out.png x,y,w,h[;x,y,w,h] q1 q2 ...
"""
import sys

import numpy as np
from PIL import Image, ImageDraw

d, W, H, out = sys.argv[1], int(sys.argv[2]), int(sys.argv[3]), sys.argv[4]
regions = [tuple(int(v) for v in r.split(',')) for r in sys.argv[5].split(';')]
qs = sys.argv[6:]
BLACK, WB = 1008.0, np.array([2.1, 1.0, 1.55])


def demosaic(a):
    a = a.astype(np.float32) - BLACK
    h, w = a.shape
    rgb = np.zeros((h, w, 3), np.float32)
    mask = np.zeros((h, w, 3), np.float32)
    rgb[0::2, 0::2, 0] = a[0::2, 0::2]; mask[0::2, 0::2, 0] = 1
    rgb[0::2, 1::2, 1] = a[0::2, 1::2]; mask[0::2, 1::2, 1] = 1
    rgb[1::2, 0::2, 1] = a[1::2, 0::2]; mask[1::2, 0::2, 1] = 1
    rgb[1::2, 1::2, 2] = a[1::2, 1::2]; mask[1::2, 1::2, 2] = 1
    k = np.array([[1, 2, 1], [2, 4, 2], [1, 2, 1]], np.float32)
    out = np.zeros_like(rgb)
    for c in range(3):
        num = np.zeros((h, w), np.float32)
        den = np.zeros((h, w), np.float32)
        pv, pm = np.pad(rgb[..., c], 1, mode='edge'), np.pad(mask[..., c], 1, mode='edge')
        for dy in range(3):
            for dx in range(3):
                num += k[dy, dx] * pv[dy:dy + h, dx:dx + w]
                den += k[dy, dx] * pm[dy:dy + h, dx:dx + w]
        out[..., c] = np.where(mask[..., c] > 0, rgb[..., c], num / np.maximum(den, 1e-6))
    return out


def render(a, scale):
    x = np.clip(demosaic(a) * WB * scale, 0, 1) ** (1 / 2.2)
    return Image.fromarray((x * 255).astype(np.uint8))


tiles = []
for (x, y, w, h) in regions:
    orig = np.fromfile(f'{d}/orig.u16', '<u2').reshape(H, W)[y:y + h + 2, x:x + w + 2]
    scale = 1.0 / np.percentile((orig.astype(np.float32) - BLACK) * 2.0, 99.5)
    row = []
    for q in qs:
        a = np.fromfile(f'{d}/q{q}.u16', '<u2').reshape(H, W)[y:y + h + 2, x:x + w + 2]
        im = render(a, scale).crop((1, 1, w + 1, h + 1))
        dr = ImageDraw.Draw(im)
        dr.rectangle((0, 0, 58, 18), fill=(0, 0, 0))
        dr.text((4, 3), 'q' + q if q != '100' else 'lossless', fill=(255, 255, 0))
        row.append(im)
    tiles.append(row)
cols = len(qs)
tw, th = regions[0][2], regions[0][3]
sheet = Image.new('RGB', (cols * (tw + 4), len(regions) * (th + 4)), 'white')
for r, row in enumerate(tiles):
    for c, im in enumerate(row):
        sheet.paste(im, (c * (tw + 4), r * (th + 4)))
sheet.save(out)

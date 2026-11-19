"""Extract the raw CFA (or linear) image from an uncompressed DNG written by Adobe DNG Converter.

usage: python dngraw.py in.dng out.u16      -> prints geometry/photometric; writes uint16 LE samples
"""
import struct
import sys

import numpy as np

sys.path.insert(0, __file__.rsplit('\\', 1)[0] if '\\' in __file__ else '.')
from tiffdump import T  # noqa: E402

d = open(sys.argv[1], 'rb').read()
t = T(d)


def tags(off):
    ents, nxt = t.ifd(off)
    return {e[0]: e for e in ents}, nxt


def val(e, n=1 << 20):
    return t.val(e[1], e[2], e[3], maxn=n)


ifds = []
o = t.u32(4)
while o:
    tg, o2 = tags(o)
    ifds.append(tg)
    if 0x14a in tg:
        for so in val(tg[0x14a]):
            ifds.append(tags(so)[0])
    o = o2
raw = [x for x in ifds if 0xfe in x and val(x[0xfe])[0] == 0][0]
w, h = val(raw[0x100])[0], val(raw[0x101])[0]
spp = val(raw[0x115])[0] if 0x115 in raw else 1
bps = val(raw[0x102])[0]
comp = val(raw[0x103])[0]
phot = val(raw[0x106])[0]
print(f'{w}x{h} spp={spp} bps={bps} compression={comp} photometric={phot}')
for k, name in [(0xc61a, 'BlackLevel'), (0xc61d, 'WhiteLevel'), (0xc68d, 'ActiveArea'), (0xc618, 'LinearizationTable')]:
    if k in raw:
        print(' ', name, val(raw[k], 8))
assert comp == 1 and bps == 16
if 0x144 in raw:   # tiles
    tw, th = val(raw[0x142])[0], val(raw[0x143])[0]
    offs, cnts = val(raw[0x144]), val(raw[0x145])
    img = np.zeros((h, w * spp), np.uint16)
    tx = (w + tw - 1) // tw
    for i, (to, tc) in enumerate(zip(offs, cnts)):
        ty, txi = divmod(i, tx)
        tile = np.frombuffer(d, '<u2' if t.bo == '<' else '>u2', tw * th * spp, to).reshape(th, tw * spp)
        y0, x0 = ty * th, txi * tw * spp
        hh, ww = min(th, h - y0), min(tw * spp, w * spp - x0)
        img[y0:y0 + hh, x0:x0 + ww] = tile[:hh, :ww]
else:
    offs, cnts = val(raw[0x111]), val(raw[0x117])
    buf = b''.join(d[a:a + b] for a, b in zip(offs, cnts))
    img = np.frombuffer(buf, '<u2' if t.bo == '<' else '>u2', w * h * spp).reshape(h, w * spp)
img.astype('<u2').tofile(sys.argv[2])

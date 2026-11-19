"""Break down a NEF's skeleton (bytes before the raw strip) by content and compressibility.

usage: python skelmap.py file.NEF
"""
import sys
import zlib

sys.path.insert(0, __file__.rsplit('\\', 1)[0] if '\\' in __file__ else '.')
from tiffdump import T  # noqa: E402

d = open(sys.argv[1], 'rb').read()
t = T(d)
regions = []  # (start, end, label)


def add(a, n, label):
    if n > 0:
        regions.append((a, a + n, label))


def ents_of(tt, off):
    ents, nxt = tt.ifd(off)
    return {e[0]: e for e in ents}, nxt


def walk_ifd(tt, off, name, base=0):
    tg, nxt = ents_of(tt, off)
    add(tt.base + off, 2 + 12 * len(tg) + 4, f'{name} IFD entries')
    for tag, (tag_, typ, cnt, vo) in tg.items():
        sz = {1: 1, 2: 1, 3: 2, 4: 4, 5: 8, 7: 1, 8: 2, 9: 4, 10: 8, 11: 4, 12: 8}.get(typ, 1) * cnt
        if sz > 4 and tag not in (0x927c,):
            label = {0x02bc: 'XMP packet', 0xc7d5: 'Nikon 0xC7D5 block'}.get(tag, f'{name} tag values')
            add(tt.base + vo, sz, label)
    for otag, ctag, what in [(0x111, 0x117, 'strip'), (0x201, 0x202, 'JPEG')]:
        if otag in tg and ctag in tg:
            o = tt.val(*tg[otag][1:])[0]
            c = tt.val(*tg[ctag][1:])[0]
            w = tt.val(*tg[0x100][1:])[0] if 0x100 in tg else None
            h = tt.val(*tg[0x101][1:])[0] if 0x101 in tg else None
            comp = tt.val(*tg[0x103][1:])[0] if 0x103 in tg else None
            bps = tt.val(*tg[0x102][1:])[0] if 0x102 in tg else None
            if what == 'JPEG':
                lab = f'{name} JPEG preview'
            elif comp == 34713 and w and w > 3000:
                lab = 'RAW STRIP'
            else:
                lab = f'{name} image {w}x{h} {bps}-bit comp {comp}'
            add(tt.base + o, c, lab)
    return tg, nxt


tg0, nxt = walk_ifd(t, t.u32(4), 'IFD0')
add(0, 8, 'TIFF header')
for i, so in enumerate(t.val(*tg0[0x14a][1:])):
    walk_ifd(t, so, f'SubIFD{i}')
ex = t.val(*tg0[0x8769][1:])[0]
tge, _ = walk_ifd(t, ex, 'Exif')
mn = tge[0x927c]
add(mn[3], mn[2], 'makernote (total)')
mt = T(d, mn[3] + 10)
mtg, _ = ents_of(mt, mt.u32(4))
mn_parts = []
for tag, (tag_, typ, cnt, vo) in mtg.items():
    sz = {1: 1, 2: 1, 3: 2, 4: 4, 5: 8, 7: 1, 8: 2, 9: 4, 10: 8}.get(typ, 1) * cnt
    if sz > 4:
        mn_parts.append((sz, tag))
if 0x11 in mtg:  # makernote preview IFD
    po = mt.val(*mtg[0x11][1:])[0]
    ptg, _ = ents_of(mt, po)
    if 0x201 in ptg:
        o, c = mt.val(*ptg[0x201][1:])[0], mt.val(*ptg[0x202][1:])[0]
        mn_parts.append((c, 'preview JPEG (in makernote)'))
if 0x8825 in tg0:
    walk_ifd(t, t.val(*tg0[0x8825][1:])[0], 'GPS')

strip = [r for r in regions if r[2] == 'RAW STRIP'][0]
end = strip[0]
cover = bytearray(end)
labels = {}
for a, b, lab in sorted(regions, key=lambda r: (r[2] == 'makernote (total)', r[0])):
    if lab == 'RAW STRIP' or a >= end:
        continue
    b = min(b, end)
    seg = [i for i in range(a, b) if not cover[i]] if lab == 'makernote (total)' else None
    if seg is not None:
        n = len(seg)
        data = bytes(d[i] for i in seg)
        for i in seg:
            cover[i] = 1
    else:
        n = b - a
        data = d[a:b]
        cover[a:b] = b'\1' * n
    l = labels.setdefault(lab, [0, b''])
    l[0] += n
    l[1] += data
gap = bytes(d[i] for i in range(end) if not cover[i])
labels['unreferenced (padding etc.)'] = [len(gap), gap]
print(f'skeleton {end / 1e6:.3f} MB, zlib-9 of whole: {len(zlib.compress(d[:end], 9)) / 1e6:.3f} MB')
print(f'{"content":45s} {"bytes":>10s} {"zlib":>10s}  zero-bytes')
for lab, (n, data) in sorted(labels.items(), key=lambda kv: -kv[1][0]):
    z = len(zlib.compress(data, 9))
    print(f'{lab:45s} {n:10,d} {z:10,d}  {data.count(0) / max(1, n):5.0%}')
print('makernote big tags:', ', '.join(f'{(t if isinstance(t, str) else hex(t))}={s:,}' for s, t in sorted(mn_parts, reverse=True)[:8]))

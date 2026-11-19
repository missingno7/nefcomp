"""Rebuild a genuine Nikon NEF from a JPEG XL DNG written by dngjxl.py --nef.

The DNG carries the original NEF's metadata skeleton (everything before the raw
strip: IFDs, makernote, embedded JPEG previews) in private tag 0xFE12. The raw
mosaic is decoded from the JPEG XL tiles, linearised with the DNG
LinearizationTable, and re-encoded in the NEF's original raw format (lossless /
uncompressed / HE) by `nefcomp assemble`.

usage: python dng2nef.py in.dng out.NEF [--nefcomp nefcomp.exe]
"""
import argparse
import hashlib
import os
import struct
import subprocess
import sys
import tempfile
import zlib

import imagecodecs
import numpy as np

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from tiffdump import T  # noqa: E402

SKEL_TAG = 0xFE12


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('src')
    ap.add_argument('dst')
    ap.add_argument('--nefcomp', default='nefcomp.exe')
    a = ap.parse_args()

    d = open(a.src, 'rb').read()
    t = T(d)
    ents, _ = t.ifd(t.u32(4))
    ifd0 = {e[0]: e for e in ents}
    if SKEL_TAG not in ifd0:
        raise SystemExit('this DNG has no embedded NEF skeleton (not written by dngjxl.py --nef)')
    typ, cnt, off = ifd0[SKEL_TAG][1:]
    blob = d[off:off + cnt]
    magic, ver, orig_size, orig_sha, px_sha = struct.unpack_from('<12sIQ32s32s', blob)
    if magic != b'NEFCOMP-SKEL' or ver != 1:
        raise SystemExit('unknown skeleton format')
    skel = zlib.decompress(blob[struct.calcsize('<12sIQ32s32s'):])

    raw = None
    for so in t.val(*ifd0[0x14a][1:]):
        se, _ = t.ifd(so)
        tg = {e[0]: e for e in se}
        if t.val(*tg[0xfe][1:])[0] == 0:
            raw = tg
    g = lambda k, n=1 << 20: t.val(*raw[k][1:], maxn=n)  # noqa: E731
    W, H = g(0x100)[0], g(0x101)[0]
    if g(0x103)[0] != 52546:
        raise SystemExit('raw image is not JPEG XL compressed')
    T_w, T_h = g(0x142)[0], g(0x143)[0]
    offs, cnts = g(0x144), g(0x145)
    tx, ty = (W + T_w - 1) // T_w, (H + T_h - 1) // T_h
    stored = np.zeros((ty * T_h, tx * T_w), np.uint16)
    for i, (o, c) in enumerate(zip(offs, cnts)):
        yy, xx = divmod(i, tx)
        stored[yy * T_h:(yy + 1) * T_h, xx * T_w:(xx + 1) * T_w] = imagecodecs.jpegxl_decode(d[o:o + c])
    s = stored[:H, :W]
    rif = g(0xc71f)[0] if 0xc71f in raw else 1
    cif = g(0xcd43)[0] if 0xcd43 in raw else 1
    if (rif, cif) == (2, 2):
        h2, w2 = H // 2, W // 2
        codes = np.zeros((H, W), np.uint16)
        codes[0::2, 0::2] = s[:h2, :w2]
        codes[0::2, 1::2] = s[:h2, w2:]
        codes[1::2, 0::2] = s[h2:, :w2]
        codes[1::2, 1::2] = s[h2:, w2:]
    elif (rif, cif) == (1, 1):
        codes = s.copy()
    else:
        raise SystemExit(f'unsupported interleave {rif}x{cif}')
    if 0xc618 in raw:
        lut = np.array(g(0xc618, 70000), np.uint16)
        px = lut[np.minimum(codes, len(lut) - 1)]
    else:
        px = codes
    px = np.minimum(px, 16383).astype('<u2')
    exact = hashlib.sha256(px.tobytes()).digest() == px_sha

    with tempfile.TemporaryDirectory() as td:
        sk, pp = os.path.join(td, 'skel.bin'), os.path.join(td, 'px.u16')
        open(sk, 'wb').write(skel)
        px.tofile(pp)
        subprocess.run([a.nefcomp, 'assemble', sk, pp, a.dst], check=True)
    out = open(a.dst, 'rb').read()
    same_file = hashlib.sha256(out).digest() == orig_sha
    print(f'rebuilt {a.dst}: {len(out) / 1e6:.2f} MB (original {orig_size / 1e6:.2f} MB); '
          f'pixels {"identical to original" if exact else "lossy (as compressed)"}; '
          f'file {"BYTE-IDENTICAL to original" if same_file else "differs from original only in raw data"}')


main()

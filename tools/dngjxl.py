"""Prototype: recompress the raw CFA of an uncompressed DNG with lossy JPEG XL.

The raw samples are companded with Nikon HE's square-root transfer into 16-bit
codes (a DNG LinearizationTable maps them back to linear), split per tile into
the four Bayer planes (DNG 1.7 Row/ColumnInterleaveFactor = 2), and each tile is
JPEG XL encoded.  All other IFDs/metadata are copied unchanged.

usage: python dngjxl.py in_uncompressed.dng out.dng [--distance D] [--tile N] [--effort E] [--raw-out decoded.u16]
"""
import argparse
import math
import struct
import sys

import imagecodecs
import numpy as np

TYPE_SIZE = {1: 1, 2: 1, 3: 2, 4: 4, 5: 8, 6: 1, 7: 1, 8: 2, 9: 4, 10: 8, 11: 4, 12: 8, 13: 4, 16: 8}
IFD_TAGS = {0x14a, 0x8769, 0x8825, 0xa005}
SKEL_TAG = 0xFE12   # private (reusable range): zlib'd NEF skeleton for dng2nef.py


class Reader:
    def __init__(self, d):
        self.d = d
        self.bo = '<' if d[:2] == b'II' else '>'

    def u16(self, o): return struct.unpack_from(self.bo + 'H', self.d, o)[0]
    def u32(self, o): return struct.unpack_from(self.bo + 'I', self.d, o)[0]

    def ifd(self, off):
        n = self.u16(off)
        ents = {}
        for i in range(n):
            e = off + 2 + 12 * i
            tag, typ, cnt = self.u16(e), self.u16(e + 2), self.u32(e + 4)
            sz = TYPE_SIZE.get(typ, 1) * cnt
            vo = e + 8 if sz <= 4 else self.u32(e + 8)
            ents[tag] = [typ, cnt, self.d[vo:vo + sz]]
        return ents, self.u32(off + 2 + 12 * n)

    def ints(self, ent):
        typ, cnt, raw = ent
        f = {3: 'H', 4: 'I', 13: 'I', 1: 'B'}[typ]
        return list(struct.unpack(self.bo + f * cnt, raw))


def load_tree(r, off):
    """Return list of IFD dicts: {'tags':..., 'subs': {tag: [child trees]}}."""
    ents, nxt = r.ifd(off)
    node = {'tags': ents, 'subs': {}}
    for t in IFD_TAGS & set(ents):
        node['subs'][t] = [load_tree(r, o)[0] for o in r.ints(ents[t])]
    # data blocks referenced by offsets
    node['blocks'] = {}
    for otag, ctag in [(0x111, 0x117), (0x144, 0x145), (0x201, 0x202)]:
        if otag in ents and ctag in ents:
            offs, cnts = r.ints(ents[otag]), r.ints(ents[ctag])
            node['blocks'][otag] = [r.d[o:o + c] for o, c in zip(offs, cnts)]
    chain = [node]
    if nxt:
        chain += load_tree(r, nxt)
    return chain


class Writer:
    def __init__(self, bo='<'):
        self.bo = bo
        self.out = bytearray(b'II*\0\0\0\0\0' if bo == '<' else b'MM\0*\0\0\0\0')

    def align(self):
        if len(self.out) & 1:
            self.out.append(0)

    def write_chain(self, chain):
        """Write an IFD chain; returns offset of first IFD."""
        offs = [self.write_ifd(n) for n in chain]
        for a, b in zip(offs, offs[1:]):
            n = struct.unpack_from(self.bo + 'H', self.out, a)[0]
            struct.pack_into(self.bo + 'I', self.out, a + 2 + 12 * n, b)
        return offs[0]

    def write_ifd(self, node):
        tags = dict(node['tags'])
        # children first
        for t, kids in node['subs'].items():
            child_offs = [self.write_chain([k]) for k in kids]
            tags[t] = [4 if t != 0x14a else 4, len(child_offs), struct.pack(self.bo + 'I' * len(child_offs), *child_offs)]
        # data blocks
        for otag, blocks in node['blocks'].items():
            ctag = {0x111: 0x117, 0x144: 0x145, 0x201: 0x202}[otag]
            offs = []
            for b in blocks:
                self.align()
                offs.append(len(self.out))
                self.out += b
            if otag == 0x201:
                tags[otag] = [4, 1, struct.pack(self.bo + 'I', offs[0])]
                tags[ctag] = [4, 1, struct.pack(self.bo + 'I', len(blocks[0]))]
            else:
                tags[otag] = [4, len(offs), struct.pack(self.bo + 'I' * len(offs), *offs)]
                tags[ctag] = [4, len(offs), struct.pack(self.bo + 'I' * len(offs), *[len(b) for b in blocks])]
        # value area then IFD
        keys = sorted(tags)
        vals = {}
        for k in keys:
            typ, cnt, raw = tags[k]
            if len(raw) > 4:
                self.align()
                vals[k] = len(self.out)
                self.out += raw
        self.align()
        pos = len(self.out)
        self.out += struct.pack(self.bo + 'H', len(keys))
        for k in keys:
            typ, cnt, raw = tags[k]
            self.out += struct.pack(self.bo + 'HHI', k, typ, cnt)
            self.out += struct.pack(self.bo + 'I', vals[k]) if k in vals else raw.ljust(4, b'\0')
        self.out += b'\0\0\0\0'
        return pos


def he_transfer(black):
    """Nikon HE square-root companding: code (0..65535) -> linear 14-bit value."""
    if black == 1008:
        A, B, C = 84429, 1008, 16255
    else:
        C = int(math.floor(math.sqrt(black * 262144.0)))
        A = int(round(65536 * math.sqrt((16384 - black) * 262144.0) / (65535 - C)))
        B = black
    i = np.arange(65536, dtype=np.int64)
    d = i - C
    z = (d * A + 32768) >> 16
    q = np.where(d < 0, -d * d, z * z)
    v = q + 163840
    fl = np.where(v >= 0, v >> 18, -((-v + (1 << 18) - 1) >> 18))
    return np.clip(B + fl, 0, 16383).astype(np.int64)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('src')
    ap.add_argument('dst')
    ap.add_argument('--distance', type=float, default=1.0)
    ap.add_argument('--tile', type=int, default=512)
    ap.add_argument('--effort', type=int, default=7)
    ap.add_argument('--no-companding', action='store_true')
    ap.add_argument('--raw-out')
    ap.add_argument('--nef', help='original NEF: embed its metadata skeleton so dng2nef.py can rebuild it')
    ap.add_argument('--nefcomp', default='nefcomp.exe')
    ap.add_argument('--lossless', action='store_true', help='lossless JPEG XL (exact NEF rebuild)')
    a = ap.parse_args()

    r = Reader(open(a.src, 'rb').read())
    chain = load_tree(r, r.u32(4))
    ifd0 = chain[0]
    raw_node = None
    for kid in ifd0['subs'].get(0x14a, []):
        if r.ints(kid['tags'][0xfe])[0] == 0:
            raw_node = kid
    t = raw_node['tags']
    W, H = r.ints(t[0x100])[0], r.ints(t[0x101])[0]
    assert r.ints(t[0x103])[0] == 1 and r.ints(t[0x102])[0] == 16 and r.ints(t[0x106])[0] == 32803
    data = b''.join(raw_node['blocks'][0x111])
    img = np.frombuffer(data, r.bo + 'u2', W * H).reshape(H, W).astype(np.int64)
    black_rat = struct.unpack(r.bo + 'II', t[0xc61a][2][:8])
    black = round(black_rat[0] / black_rat[1])

    skel_blob = None
    if a.nef:
        import hashlib, os, subprocess, tempfile, zlib
        with tempfile.TemporaryDirectory() as td:
            sk, px = os.path.join(td, 'skel.bin'), os.path.join(td, 'px.u16')
            subprocess.run([a.nefcomp, 'skeleton', a.nef, sk], check=True)
            subprocess.run([a.nefcomp, 'decode', a.nef, px], check=True, capture_output=True)
            skel = open(sk, 'rb').read()
            nefpx = np.fromfile(px, '<u2').astype(np.int64)
        if nefpx.size != W * H or not (nefpx.reshape(H, W) == img).all():
            raise SystemExit('DNG raw data differs from the NEF decode; refusing to embed')
        orig = open(a.nef, 'rb').read()
        hdr = struct.pack('<12sIQ32s32s', b'NEFCOMP-SKEL', 1, len(orig), hashlib.sha256(orig).digest(),
                          hashlib.sha256(img.astype('<u2').tobytes()).digest())
        skel_blob = hdr + zlib.compress(skel, 9)
        print(f'embedded NEF skeleton: {len(skel) / 1e6:.2f} MB -> {len(skel_blob) / 1e6:.2f} MB compressed')

    if a.no_companding:
        codes = img.astype(np.uint16)
        lut = None
    else:
        tr = he_transfer(black)
        # inverse: nearest code for every linear value
        inv = np.searchsorted(tr, np.arange(16384), side='left').clip(0, 65535)
        lo = (inv - 1).clip(0, 65535)
        better = np.abs(tr[lo] - np.arange(16384)) < np.abs(tr[inv] - np.arange(16384))
        inv = np.where(better & (inv > 0), lo, inv)
        codes = inv[np.clip(img, 0, 16383)].astype(np.uint16)
        lut = tr.astype(np.uint16)

    # Row/ColumnInterleaveFactor = 2 apply to the whole image (verified against
    # Adobe DNG Converter): stored image = [R G1; G2 B] quadrant planes.
    assert H % 2 == 0 and W % 2 == 0
    stored = np.block([[codes[0::2, 0::2], codes[0::2, 1::2]], [codes[1::2, 0::2], codes[1::2, 1::2]]]).astype(np.uint16)
    T = a.tile
    tiles = []
    tx, ty = (W + T - 1) // T, (H + T - 1) // T
    dec_stored = np.zeros((ty * T, tx * T), np.uint16)
    total = 0
    for yy in range(ty):
        for xx in range(tx):
            tile = np.zeros((T, T), np.uint16)
            blk = stored[yy * T:(yy + 1) * T, xx * T:(xx + 1) * T]
            tile[:blk.shape[0], :blk.shape[1]] = blk
            # replicate edges into the padding so the codec doesn't waste bits
            if blk.shape[0] < T:
                tile[blk.shape[0]:, :] = tile[blk.shape[0] - 1, :]
            if blk.shape[1] < T:
                tile[:, blk.shape[1]:] = tile[:, blk.shape[1] - 1:blk.shape[1]]
            if a.lossless:
                enc = imagecodecs.jpegxl_encode(tile, lossless=True, effort=a.effort, bitspersample=16)
            else:
                enc = imagecodecs.jpegxl_encode(tile, distance=a.distance, effort=a.effort, bitspersample=16)
            tiles.append(enc)
            total += len(enc)
            dec_stored[yy * T:(yy + 1) * T, xx * T:(xx + 1) * T] = imagecodecs.jpegxl_decode(enc)
    ds = dec_stored[:H, :W]
    h2, w2 = H // 2, W // 2
    decoded = np.zeros((H, W), np.uint16)
    decoded[0::2, 0::2] = ds[:h2, :w2]
    decoded[0::2, 1::2] = ds[:h2, w2:]
    decoded[1::2, 0::2] = ds[h2:, :w2]
    decoded[1::2, 1::2] = ds[h2:, w2:]

    dec_lin = (lut[decoded] if lut is not None else decoded).astype(np.int64)
    err = dec_lin - img
    peak = 16383 - black
    rms = math.sqrt(float((err.astype(np.float64) ** 2).mean()))
    psnr = f'PSNR {20 * math.log10(peak / rms):.2f} dB' if rms else 'EXACT'
    print(f'raw {W}x{H}: JXL tiles {total / 1e6:.2f} MB, {total * 8 / (W * H):.2f} bpp; '
          f'rms {rms:.2f} max {abs(err).max()} {psnr}')
    if a.raw_out:
        dec_lin.astype('<u2').tofile(a.raw_out)

    # rewrite raw IFD
    bo = r.bo
    for k in (0x111, 0x116, 0x117):
        t.pop(k, None)
    raw_node['blocks'] = {0x144: tiles}
    t[0x103] = [3, 1, struct.pack(bo + 'H', 52546)]          # Compression = JPEG XL
    t[0x142] = [4, 1, struct.pack(bo + 'I', T)]              # TileWidth
    t[0x143] = [4, 1, struct.pack(bo + 'I', T)]              # TileLength
    t[0xc71f] = [3, 1, struct.pack(bo + 'H', 2)]             # RowInterleaveFactor
    t[0xcd43] = [3, 1, struct.pack(bo + 'H', 2)]             # ColumnInterleaveFactor
    t[0xcd44] = [11, 1, struct.pack(bo + 'f', 0.0 if a.lossless else a.distance)]   # JXLDistance
    t[0xcd45] = [4, 1, struct.pack(bo + 'I', a.effort)]      # JXLEffort
    if lut is not None:
        t[0xc618] = [3, 65536, lut.astype(bo + 'u2').tobytes()]  # LinearizationTable
    if skel_blob is not None:
        ifd0['tags'][SKEL_TAG] = [7, len(skel_blob), skel_blob]
    # NewRawImageDigest: for JPEG XL raw data Adobe uses MD5 over the per-tile
    # MD5s of the compressed tiles (verified on a DNG Converter lossy DNG).
    # A stale digest makes Camera Raw report the file as damaged.
    import hashlib
    ifd0['tags'][0xc7a7] = [1, 16, hashlib.md5(b''.join(hashlib.md5(x).digest() for x in tiles)).digest()]
    ifd0['tags'].pop(0xc71c, None)                           # legacy RawImageDigest
    ifd0['tags'][0xc612] = [1, 4, bytes([1, 7, 0, 0])]       # DNGVersion
    ifd0['tags'][0xc613] = [1, 4, bytes([1, 7, 0, 0])]       # DNGBackwardVersion

    w = Writer(bo)
    first = w.write_chain(chain)
    struct.pack_into(bo + 'I', w.out, 4, first)
    open(a.dst, 'wb').write(w.out)
    print(f'wrote {a.dst}: {len(w.out) / 1e6:.2f} MB')


main()

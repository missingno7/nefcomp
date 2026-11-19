"""Create experimental NEF variants (metadata surgery) for decoder acceptance tests.

usage: python nefexp.py in.NEF out.NEF op [op ...]
ops:
  model=NAME            replace IFD0 Model (same length incl. NUL padding)
  mnrm=TAG              remove makernote entry (hex tag)
  c7rm=TAG              remove entry from raw-IFD 0xC7D5 Nikon block (hex tag)
  mnbyte=TAG:OFF:VAL    set byte OFF of makernote tag TAG to VAL
  rawrm=TAG             remove entry from raw IFD
"""
import struct
import sys

sys.path.insert(0, __file__.rsplit('\\', 1)[0] if '\\' in __file__ else '.')
from tiffdump import T  # noqa: E402


def remove_entry(d, t, ifd_off, tag):
    n = t.u16(ifd_off)
    base = t.base + ifd_off + 2
    for i in range(n):
        e = base + 12 * i
        if t.u16(ifd_off + 2 + 12 * i) == tag:
            rest = d[e + 12:base + 12 * n + 4]
            d[e:e + len(rest)] = rest
            d[base + 12 * (n - 1) + 4:base + 12 * n + 4] = b'\0' * 12
            struct.pack_into(t.bo + 'H', d, t.base + ifd_off, n - 1)
            return True
    raise SystemExit(f'tag {tag:#x} not found')


def find(t, ifd_off, tag):
    ents, _ = t.ifd(ifd_off)
    for e in ents:
        if e[0] == tag:
            return e
    return None


def main():
    src, dst, ops = sys.argv[1], sys.argv[2], sys.argv[3:]
    d = bytearray(open(src, 'rb').read())
    for op in ops:
        t = T(bytes(d))
        ifd0 = t.u32(4)
        sub = t.val(*find(t, ifd0, 0x14a)[1:])
        raw = [o for o in sub if (find(t, o, 0xfe) and t.val(*find(t, o, 0xfe)[1:])[0] == 0)][0]
        exif = t.val(*find(t, ifd0, 0x8769)[1:])[0]
        mn = find(t, exif, 0x927c)
        mt = T(bytes(d), mn[3] + 10)
        mifd = mt.u32(4)
        k, v = op.split('=', 1)
        if k == 'model':
            e = find(t, ifd0, 0x110)
            o = e[3]
            new = v.encode().ljust(e[2], b'\0')
            assert len(new) == e[2], 'model length mismatch'
            d[o:o + e[2]] = new
        elif k == 'mnrm':
            remove_entry(d, mt, mifd, int(v, 16))
        elif k == 'rawrm':
            remove_entry(d, t, raw, int(v, 16))
        elif k == 'c7rm':
            e = find(t, raw, 0xc7d5)
            ct = T(bytes(d), e[3] + 10)
            remove_entry(d, ct, ct.u32(4), int(v, 16))
        elif k == 'mnbyte':
            tag, off, val = v.split(':')
            e = find(mt, mifd, int(tag, 16))
            d[mt.base + e[3] + int(off)] = int(val, 0)
        else:
            raise SystemExit('unknown op ' + op)
    open(dst, 'wb').write(d)


main()

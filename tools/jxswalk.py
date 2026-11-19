"""Walk the precinct structure of a Nikon HE codestream (.jxs extracted strip).

Prints per-precinct Lprc, Q, R, Dpb bytes, the 8 sub-packet headers
(data/gcli/sign lengths) and how many trailing zero bytes each precinct has.
usage: python jxswalk.py file.jxs [width] [max_precincts]
"""
import sys
import struct

d = open(sys.argv[1], 'rb').read()
W = int(sys.argv[2]) if len(sys.argv) > 2 else 8280
maxp = int(sys.argv[3]) if len(sys.argv) > 3 else 40
i = 2
while True:
    m, l = struct.unpack('>HH', d[i:i + 4])
    i += 2 + l
    if m == 0xff20:
        break
sig_bytes = (W // 2 + 255) // 256
p = 0
stats = []
while i < len(d) - 2:
    sz = int.from_bytes(d[i:i + 3], 'big')
    Q, R = d[i + 3], d[i + 4]
    dpb = d[i + 5:i + 12].hex()
    j = i + 12
    subs = []
    for lb in range(8):
        mini = int.from_bytes(d[j:j + 7], 'big')
        dl, gl, sl = mini >> 35, (mini >> 15) & 0xfffff, mini & 0x7fff
        subs.append((dl, gl, sl))
        j += 7 + sig_bytes + dl + gl + sl
    end = i + 12 + sz
    used = j - i
    tail = d[j:end]
    nz = len(tail) - len(tail.rstrip(b'\0'))
    stats.append((p, sz + 12, Q, R, used, end - j, nz))
    if p < maxp:
        print(f'P{p:4d} @{i:#x} total={sz + 12} Q={Q} R={R} dpb={dpb} used={used} pad={end - j} (zero tail {nz}) subs={subs}')
    i = end
    p += 1
    if p % 16 == 0 and d[i:i + 2] == b'\xff\x20':
        if p <= maxp:
            print('   SLH', d[i:i + 6].hex())
        i += 6
print('precincts', p, 'final bytes', d[i:].hex())
qs = {}
for s in stats:
    qs[(s[2], s[3])] = qs.get((s[2], s[3]), 0) + 1
print('Q/R histogram', sorted(qs.items()))
pads = [s[5] for s in stats]
print('pad min/max/mean', min(pads), max(pads), sum(pads) / len(pads))
print('total sizes min/max', min(s[1] for s in stats), max(s[1] for s in stats))
print('size mod 16 histogram', {k: sum(1 for s in stats if s[1] % 16 == k) for k in range(16) if any(s[1] % 16 == k for s in stats)})

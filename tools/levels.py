"""Quality-level analysis: compare decoded levels against the source and its own noise.

usage: python levels.py dir W H black q1 q2 ...   (dir holds orig.u16 and q<N>.u16)
"""
import sys

import numpy as np

d, W, H, black = sys.argv[1], int(sys.argv[2]), int(sys.argv[3]), int(sys.argv[4])
qs = sys.argv[5:]
o = np.fromfile(f'{d}/orig.u16', '<u2').reshape(H, W).astype(np.float32)
lin = np.clip(o - black, 0, None)
# noise estimate: same-colour horizontal second difference, robust (MAD), per brightness band
r = o[:, 2:-2] - (o[:, :-4] + o[:, 4:]) / 2
lb = lin[:, 2:-2]
edges = [0, 64, 256, 1024, 4096, 16384]
bands = list(zip(edges[:-1], edges[1:]))
noise, frac = [], []
for lo, hi in bands:
    m = (lb >= lo) & (lb < hi)
    rr = r[m]
    sigma = 1.4826 * np.median(np.abs(rr - np.median(rr))) / np.sqrt(1.5) if rr.size > 1000 else np.nan
    noise.append(sigma)
    frac.append(m.mean())
print('brightness band (DN above black): ' + '  '.join(f'{lo}-{hi}' for lo, hi in bands))
print('share of pixels               : ' + '  '.join(f'{f * 100:5.1f}%' for f in frac))
print('sensor noise sigma (DN)       : ' + '  '.join(f'{s:6.1f}' for s in noise))
print()
print('level | rms error per band (DN)                  | error / noise per band          | overall rms')
for q in qs:
    t = np.fromfile(f'{d}/q{q}.u16', '<u2').reshape(H, W).astype(np.float32)
    e = (t - o)[:, 2:-2]
    rms, ratio = [], []
    for (lo, hi), s in zip(bands, noise):
        m = (lb >= lo) & (lb < hi)
        v = float(np.sqrt((e[m] ** 2).mean())) if m.any() else np.nan
        rms.append(v)
        ratio.append(v / s if s else np.nan)
    tot = float(np.sqrt((e ** 2).mean()))
    print(f'q{q:>4} | ' + ' '.join(f'{v:7.2f}' for v in rms) + ' | ' + ' '.join(f'{v:5.2f}' for v in ratio) + f' | {tot:6.2f}')

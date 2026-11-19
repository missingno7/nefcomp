"""Compare two uint16 RGGB dumps: error stats overall, per CFA channel and per brightness.

usage: python compare.py ref.u16 test.u16 [W H] [--black 1008]
"""
import sys
import numpy as np

args = sys.argv[1:]
black = 1008
if '--black' in args:
    i = args.index('--black'); black = int(args[i + 1]); del args[i:i + 2]
W, H = (int(args[2]), int(args[3])) if len(args) > 3 else (8280, 5520)
a = np.fromfile(args[0], dtype='<u2').reshape(H, W).astype(np.int32)
b = np.fromfile(args[1], dtype='<u2').reshape(H, W).astype(np.int32)
d = b - a
peak = 16383 - black
mse = float((d.astype(np.float64) ** 2).mean())
print(f'max|err|={np.abs(d).max()} mean|err|={np.abs(d).mean():.3f} rms={mse ** .5:.3f} bias={d.mean():.4f} '
      + (f'PSNR={10 * np.log10(peak * peak / mse):.2f} dB' if mse else 'IDENTICAL') + f' (peak {peak})')
names = ['R', 'G1', 'G2', 'B']
for k, (y, x) in enumerate([(0, 0), (0, 1), (1, 0), (1, 1)]):
    dd = d[y::2, x::2].astype(np.float64)
    print(f'  {names[k]:2s} rms={np.sqrt((dd ** 2).mean()):.3f} bias={dd.mean():.4f}')
# error relative to expected shot noise, by brightness bucket
lin = np.clip(a - black, 0, None)
edges = [0, 16, 64, 256, 1024, 4096, 16384]
for lo, hi in zip(edges[:-1], edges[1:]):
    m = (lin >= lo) & (lin < hi)
    if m.sum() < 1000:
        continue
    dd = d[m].astype(np.float64)
    print(f'  signal {lo:5d}-{hi:5d}: n={m.sum():9d} rms={np.sqrt((dd ** 2).mean()):8.3f} bias={dd.mean():+.3f}')
print('rows with error in last 4 rows:', [float(np.abs(d[r]).mean()) for r in range(H - 4, H)])

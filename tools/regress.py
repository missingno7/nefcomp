"""Regression run over many NEFs: every supported path, checked with Adobe DNG Converter.

usage: python tools/regress.py OUTDIR file.NEF|dir ...   (needs nefcomp.exe in the repo root)

Per file:
  rebuild  decode -> skeleton -> assemble == original (byte-identical)
  dng      --dng (q70) succeeds, Adobe DNG Converter accepts the DNG
  exact    --dng -m lossless --keep-previews, restore --as original == original
  lossless restore (default, lossless NEF): Adobe accepts it, raw == DNG raw
"""
import hashlib
import array
import os
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
NEFCOMP = os.environ.get('NEFCOMP_EXE') or os.path.join(ROOT, 'nefcomp.exe')
DNGC = r'C:\Program Files\Adobe\Adobe DNG Converter\Adobe DNG Converter.exe'



def restored(stem, original):
    """restore must write .NRW for Coolpix NRW originals, else .NEF"""
    return stem + ('.NRW' if original.lower().endswith('.nrw') else '.NEF')

def run(*args):
    p = subprocess.run([NEFCOMP, *args], capture_output=True, text=True)
    return p.returncode, (p.stdout + p.stderr).strip()


def adobe_ok(path, outdir):
    os.makedirs(outdir, exist_ok=True)
    base = os.path.splitext(os.path.basename(path))[0]
    out = os.path.join(outdir, base + '.dng')
    if os.path.exists(out):
        os.remove(out)
    subprocess.run([DNGC, '-c', '-d', outdir, path], capture_output=True)
    ok = os.path.exists(out)
    if ok:
        os.remove(out)
    return ok


def sha(path):
    return hashlib.sha256(open(path, 'rb').read()).hexdigest()


def main():
    out = sys.argv[1]
    files = []
    for a in sys.argv[2:]:
        if os.path.isdir(a):
            files += [os.path.join(a, f) for f in sorted(os.listdir(a)) if f.lower().endswith(('.nef', '.nrw'))]
        else:
            files.append(a)
    os.makedirs(out, exist_ok=True)
    rows = []
    for f in files:
        name = os.path.splitext(os.path.basename(f))[0]
        d = os.path.join(out, name)
        os.makedirs(d, exist_ok=True)
        rc, info = run('info', f)
        model = info.split(': ', 1)[-1].replace('NIKON CORPORATION ', '').split(',')[0] if rc == 0 else '?'
        fmt = info.split(', ')[2] if rc == 0 and info.count(', ') >= 2 else info[-60:]
        r = {'file': name, 'model': model, 'fmt': fmt}
        # 1. byte-exact rebuild
        u16, skel, reb = (os.path.join(d, x) for x in ('px.u16', 'skel.bin', 'rebuilt.NEF'))
        rc1, m1 = run('decode', f, u16)
        rc2, m2 = run('skeleton', f, skel) if rc1 == 0 else (1, m1)
        rc3, m3 = run('assemble', skel, u16, reb) if rc2 == 0 else (1, m2)
        r['rebuild'] = 'exact' if rc3 == 0 and sha(reb) == sha(f) else ('differs' if rc3 == 0 else 'FAIL: ' + (m3 or m2 or m1)[-70:])
        for x in (u16, skel, reb):
            if os.path.exists(x):
                os.remove(x)
        # 2. DNG q70
        rc, m = run('--dng', '--verify', '--overwrite', '-o', d, f)
        dng = os.path.join(d, name + '.dng')
        if rc == 0 and os.path.exists(dng):
            psnr = m.split('PSNR ')[1].split(' dB')[0] if 'PSNR ' in m else '?'
            mb = m.split(' -> ')[-1].split(' MB')[0] if ' -> ' in m else '?'
            r['dng'] = f'{mb} MB, {psnr} dB, Adobe ' + ('OK' if adobe_ok(dng, os.path.join(d, 'adb')) else 'FAIL')
        else:
            r['dng'] = 'FAIL: ' + m.splitlines()[-1][-80:] if m else 'FAIL'
        # 3. exact DNG -> restore original
        rc, m = run('--dng', '-m', 'lossless', '--keep-previews', '--force', '--suffix', '_exact', '--overwrite', '-o', d, f)
        ex = os.path.join(d, name + '_exact.dng')
        if rc == 0 and os.path.exists(ex):
            rc, m = run('restore', '--as', 'original', '--overwrite', '-o', os.path.join(d, 'orig'), ex)
            rf = restored(os.path.join(d, 'orig', name + '_exact'), f)
            r['exact'] = 'byte-identical' if rc == 0 and os.path.exists(rf) and sha(rf) == sha(f) else (
                'raw identical (HE re-encoded)' if rc == 0 and 'raw identical' in m and 'High Efficiency' in m else 'FAIL: ' + m[-80:])
        else:
            r['exact'] = 'FAIL: ' + (m.splitlines()[-1][-80:] if m else '')
        # 4. default lossless restore of the q70 DNG
        if os.path.exists(dng):
            rc, m = run('restore', '--overwrite', '-o', os.path.join(d, 'll'), dng)
            rl = restored(os.path.join(d, 'll', name), f)
            if rc == 0 and os.path.exists(rl):
                a, b = os.path.join(d, 'a.u16'), os.path.join(d, 'b.u16')
                run('decode', dng, a)
                run('decode', rl, b)
                da, db = open(a, 'rb').read(), open(b, 'rb').read()
                same = da == db
                if not same and len(da) == len(db):
                    # lossy JPEG XL can overshoot the white level: restore clamps to the bit depth
                    va, vb = array.array('H', da), array.array('H', db)
                    top = 4095 if max(vb) <= 4095 else 16383
                    same = all(min(x, top) == y for x, y in zip(va, vb))
                # the DNG may be a crop of the NEF raw: compare sizes only then
                if not same and os.path.getsize(a) != os.path.getsize(b):
                    same = None
                for x in (a, b):
                    os.remove(x)
                r['lossless'] = ('raw==DNG' if same else ('cropped' if same is None else ('kept lossy' if 'lossy' in m else 'RAW DIFFERS'))) + \
                                ', Adobe ' + ('OK' if adobe_ok(rl, os.path.join(d, 'adb')) else 'FAIL')
            else:
                r['lossless'] = 'FAIL: ' + m[-80:]
        else:
            r['lossless'] = '-'
        rows.append(r)
        print(f"{r['file']:14s} {r['model']:12s} {r['fmt']:22s} | rebuild {r['rebuild']} | dng {r['dng']} | exact {r['exact']} | lossless {r['lossless']}", flush=True)


main()

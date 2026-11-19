# nefcomp

Shrinks Nikon NEF raw files after shooting:

* **lossless NEF → High Efficiency (HE) / High Efficiency★ (HE★) NEF**, exactly the format the
  camera writes. For cameras with HE (verified on the Z 8).
* **any NEF → JPEG XL raw DNG** for cameras without HE (e.g. Z 6). The DNG keeps the raw mosaic
  and carries the original NEF's metadata, so `nefcomp restore` rebuilds a genuine Nikon NEF from it.

```
nefcomp [options] <file.NEF | dir> ...          lossless NEF -> HE / HE* NEF
nefcomp --dng [options] <file.NEF | dir> ...    NEF -> JPEG XL raw DNG (restorable)
nefcomp --auto [options] <file.NEF | dir> ...   per file: HE NEF where possible, DNG otherwise
nefcomp restore [options] <file.dng | dir> ...  DNG made by --dng -> NEF in its original format
nefcomp info <file.NEF> ...

  -r, --recursive        include subfolders (with -o, the folder structure is mirrored)
  -m, --mode MODE        hestar (default) | he | lossless (lossless: --dng / --auto only)
      --dng              write a JPEG XL DNG instead of an HE NEF (needs Adobe DNG Converter)
  DNG options:
  -q, --quality Q        1..100: 100 = lossless, 70 = hestar (default), 55 = he
      --bpp B            aim for B bits per raw pixel instead (HE* = 5, HE = 3)
      --effort E         JPEG XL effort 1..9 (default 7)
      --keep-previews    store the camera JPEG previews (lossless JPEG recompression);
                         with -m lossless, restore gives a byte-identical NEF.
                         Default: drop them, restore renders new ones from the raw
      --distance D       raw JPEG XL distance (expert; overrides --quality)
      --dng-converter P  path to "Adobe DNG Converter.exe"
  -o, --output PATH      output file (single input) or directory
      --suffix TEXT      output suffix (default _HEs / _HE for NEF, none for DNG)
      --verify           report the error against the original samples
      --overwrite        replace existing outputs (default: skip them)
      --force            --dng: also convert files that are already HE (usually larger)
      --allow-untested   allow HE NEF output for cameras other than the Z 8
  -j, --threads N        encoder threads
```

Examples:

```
nefcomp -m hestar --verify D:\Photos\Z8            # Z 8 folder -> *_HEs.NEF
nefcomp --dng --verify -o D:\Photos\dng D:\Photos\Z6
nefcomp --auto -r -o E:\Archive F:\DCIM            # whole card, mixed cameras
nefcomp restore -r -o D:\Photos\back E:\Archive
```

### Batch conversion

Pass folders and/or files; `-r` includes subfolders. nefcomp first **plans** every file, then runs
the plan:

| mode | lossless NEF, HE camera (Z 8) | lossless / uncompressed NEF, other camera | already HE / HE★ | lossy NEF |
| --- | --- | --- | --- | --- |
| default | HE NEF | skip (hint: `--auto` / `--dng`) | skip | skip |
| `--dng` | DNG | DNG | skip (`--force` converts) | skip |
| `--auto` | HE NEF | DNG | skip | skip |

* **Outputs:** without `-o` they go next to the originals; with `-o DIR` they go into DIR, and with `-r` the
  input folder structure is mirrored (`DCIM\100NCZ_8\…`). An `-o` path without an extension is a folder.
* **Re-runs:** existing outputs are skipped (`--overwrite` replaces them), so re-running a batch only does new
  files. Our own HE outputs are HE files, so they're skipped too; the output folder is never re-scanned.
* **Safety:** originals are never modified. Each output is written as `.part` and renamed when complete.
* **Report:** skips are reported with a reason and don't count as errors; only real failures give exit code 1.
  The summary shows the total saving:

```
9 file(s): 2 -> HE NEF, 2 -> JPEG XL DNG, 5 skipped
  HE   ...\100NCZ_8\DSC_7338.NEF -> E:\Archive\DCIM\100NCZ_8\DSC_7338_HEs.NEF: 56.7 MB -> 33.5 MB (HE*)
  skip ...\100NCZ_8\DSC_7339.NEF: already High Efficiency*
  DNG  ...\101NZ_6\DSC_1208.NEF -> E:\Archive\DCIM\101NZ_6\DSC_1208.dng: 30.2 MB -> 13.6 MB (...)
  skip ...\101NZ_6\DSC_1207.NEF: unsupported raw format (lossy compressed)
done: 4 converted (158.2 MB -> 75.2 MB, saved 83.0 MB = 52%), 5 skipped, 0 failed
  skipped 4 x already High Efficiency
  skipped 1 x unsupported raw format
```

`--auto` writes HE NEFs only for the Z 8 (the verified model). Other HE-capable bodies (Z 9, Z f, …)
get DNGs unless `--allow-untested` is given. Files run one after another, each using all cores.

### HE vs JPEG XL DNG equivalents

`-m hestar` / `-m he` with `--dng` mean **the same quality as HE★ / HE** (`-q 70` / `-q 55`),
measured against the lossless originals. Error is the rms difference in 14-bit raw units.

| file | HE★ NEF (5.00 bpp) | DNG `-q 70` | DNG `--bpp 5` | HE NEF (3.00 bpp) | DNG `-q 55` | DNG `--bpp 3` |
| --- | --- | --- | --- | --- | --- | --- |
| Z 8 FX DSC_7338 (56.7 MB) | 33.5 MB, err 5.6 | 28.9 MB, 4.93 bpp, err 5.5 | 29.4 MB, err 5.1 | 22.1 MB, err 18.8 | 20.8 MB, 3.51 bpp, err 14.2 | 17.9 MB, err 19.0 |
| Z 8 DX DSC_7341 (24.5 MB) | 14.4 MB, err 6.1 | 12.1 MB, 4.69 bpp, err 5.8 | 13.2 MB, err 4.1 | 9.5 MB, err 20.8 | 8.7 MB, 3.30 bpp, err 16.2 | 8.1 MB, err 18.8 |
| Z 6 DSC_1208 (30.2 MB) | (no HE) err 4.4 | 13.6 MB, 4.04 bpp, err 5.6 | 16.9 MB, err 2.5 | (no HE) err 12.7 | 9.4 MB, 2.67 bpp, err 14.7 | 10.6 MB, err 11.4 |

* Equal quality: JPEG XL needs 2–20 % less raw data than HE★, and about the same as HE.
* Equal size (`--bpp`): JPEG XL has equal or lower average error. Its worst-case pixel errors
  are larger, though (e.g. 105 vs 80 DN): JPEG XL puts error on sharp edges, while HE spreads it evenly.
* NEF sizes include ~4.9 MB (Z 8) of camera previews. DNGs carry ~0.7 MB of Adobe metadata,
  and their previews are regenerated on restore.

**bpp** = compressed raw-data bits ÷ sensor pixels (metadata and previews excluded). 1 bpp is
5.71 MB on a Z 8 FX, 2.44 MB on Z 8 DX and 3.06 MB on a Z 6. Uncompressed is 14 bpp, Nikon
lossless ~8–11, HE★ 5 and HE 3. HE and `--bpp` are constant size: busy scenes lose more. `--bpp`
bisects the JPEG XL distance on every 5th tile and lands within ~3 %. `-q` is constant quality:
the size follows the content, e.g. `-q 70` took 4.0–4.9 bpp on the files above.

**Choosing JPEG XL settings:** `-q` is the main knob; error rms ≈ 100 × distance (q 90 ≈ 1.3 DN,
q 80 ≈ 2.2, q 70 ≈ 5, q 55 ≈ 15), against sensor noise of tens of DN in midtones. A level sweep
on a detailed scene (DSC_4500) showed no visible difference down to q 50 at 100 %. Use `--bpp`
for a predictable size. `--effort 9` measured 3 % smaller but 0.4 dB worse and 1.7× slower, so
the default 7 is fine. `--keep-previews` costs 0.8 MB (Z 6) to 3.5 MB (Z 8) and makes
exact restores possible; `-m lossless --keep-previews` restores byte-identical NEFs.

## Build

MSYS2 MinGW-w64 g++: `mingw32-make` produces one static `nefcomp.exe`.
`third_party/libjxl` holds a static libjxl 0.11.1 built with the same compiler
(CMake + Ninja from Visual Studio, `-DBUILD_SHARED_LIBS=OFF`, tools/tests off).
`--dng` needs Adobe DNG Converter installed; it is used to produce Adobe's colour profile and metadata.

## HE / HE★ NEF

HE / HE★ is intoPIX TicoRAW: a JPEG XS-style codestream (ISO 21122 markers
SOC/CAP/PIH/CDT/WGT/SLH/EOC). It uses a Star-Tetrix-like CFA transform into 4 components,
one vertical and five horizontal 5/3 wavelet levels, and square-root companding
(PIH vendor extension: B = black level, C = ⌊√(B·2¹⁸)⌋, A = gain).
HE and HE★ share an identical header; only the fixed rate differs:

* codestream size `Lcod = ⌊W·H·bpp/8/512⌋·512`, where bpp = 5 for HE★ and 3 for HE
* one precinct = 4 sensor rows, 16 precincts per slice; every precinct gets
  `⌊(k+1)·P/N⌋ − ⌊k·P/N⌋` bytes. The encoder picks the best JPEG XS (Q, R) pair that fits
  and zero-pads the rest, exactly like the camera.

Container changes, derived by diffing lossless / HE / HE★ files shot back to back:

| where | change |
| --- | --- |
| raw IFD 0x0117 | strip byte count → Lcod |
| raw IFD 0xC7D5 (Nikon TIFF) | 0x000D mode, 0x000F size (each with CRC-16/CCITT-FALSE, LE); 0x000E removed |
| makernote 0x0051 byte 10 | 3 → 13 (HE) / 14 (HE★) |
| makernote 0x0058 | Lcod, 1 |
| makernote 0x0096 | removed (lossless linearization table) |

The picture header is byte-identical to the Z 8's (FX and DX, firmware 3.10 and 4.06).
Adobe DNG Converter decodes the output bit-identically to the bundled public decoder, and the files
open in Camera Raw.

**Cameras without an HE mode can't use this.** Adobe and LibRaw enable HE decoding only for models
that shoot HE (Z 9, Z 8, Z f, Z6III, Z50II, Z5II, ZR). A Z 8 HE file relabelled "NIKON Z 6" is
rejected; Z 6 data relabelled "NIKON Z 8" is accepted. Adobe also requires raw-IFD 0xC7D5/0x000D.
nefcomp refuses such models and points to `--dng`.

## JPEG XL raw DNG (`--dng`)

1. Adobe DNG Converter `-u` writes an uncompressed DNG (colour profile, opcodes, Exif). nefcomp
   checks that its raw samples equal its own NEF decode, then replaces only the raw IFD.
2. Companding: the mosaic goes through HE's square-root transfer into 16-bit codes, with a
   LinearizationTable to map back (exactly invertible).
3. Plane split: `Row/ColumnInterleaveFactor = 2` splits it into the four Bayer planes. This applies
   to the **whole image**, not per tile (verified against Adobe).
4. Compression: each 512×512 tile is a JPEG XL codestream (DNG 1.7, compression 52546), encoded in parallel.
5. Digest: `NewRawImageDigest` = MD5 over the per-tile MD5s of the compressed tiles (Adobe's rule
   for JPEG XL). A stale digest makes Camera Raw report the file as damaged.
6. Restore data: private IFD0 tag 0xFE12 holds the rest of the NEF (format v2, `src/nef_embed.cpp`),
   with SHA-256 of the original file and samples:
   * camera JPEG previews and RGB thumbnails: dropped, and regenerated on restore from the raw at the
     same size and Nikon's JPEG format (baseline 4:2:2), fitted into the original slot so no other
     byte moves. With `--keep-previews` they're stored with JPEG XL lossless JPEG recompression instead
     (bit-exact, −17…−49 %);
   * makernote (incl. its own preview): not stored, because Adobe's DNGPrivateData copy is byte-identical;
   * Nikon helper images (Z 6: 758×505 12-bit 2-channel, 1520×1010 6-bit CFA): unpacked to planes
     and stored with lossless JPEG XL (2.3 MB → 0.68 MB);
   * everything else (IFDs, Exif, XMP, padding): Brotli.

   | embedded NEF data | v1 (zlib) | v2 | v2 + `--keep-previews` |
   | --- | --- | --- | --- |
   | Z 6 | 2.86 MB | 0.69 MB | 1.50 MB |
   | Z 8 | 4.73 MB | 0.01 MB | 3.53 MB |

   What regeneration loses: the camera's own rendering of the previews (Picture Control look),
   which NX Studio browsing, Windows thumbnails and the camera's playback show. Raw data and all
   metadata are unaffected. v1 DNGs are still restorable.

`nefcomp restore` decodes the tiles and re-encodes them in the NEF's original raw format (lossless,
uncompressed or HE), then reattaches the skeleton. Nikon's lossless encoder is reproduced exactly:
decoding and re-encoding gives a byte-identical file for every sample NEF (the camera pads the
last byte with 1-bits). Results:

* With `-m lossless --keep-previews`, the restored NEF is byte-identical to the original.
* With lossy modes, the restored NEF is a genuine Nikon file that differs only in StripByteCounts
  and the pixel values. A restored Z 6 NEF opens in Camera Raw and was developed in-camera by the Z 6.

Adobe DNG Converter accepts all DNGs and restored NEFs, and its decode of the DNG equals nefcomp's.

## Tools

* `tools/tiffdump.py`, `jxswalk.py`: inspect NEF/DNG structure and HE precincts.
* `tools/compare.py`, `dngraw.py`, `preview.py`: sample-level comparisons.
* `tools/levels.py`, `levelcrops.py`, `skelmap.py`: quality-level error vs. sensor noise, 100 % crop
  sheets, and what a NEF's non-raw bytes contain. `nefcomp decode` also decodes nefcomp DNGs.
* `tools/dngtest.sh`: Adobe DNG Converter accept/reject test.
* `tools/nefexp.py`: metadata surgery for experiments.
* `tools/dngjxl.py`, `dng2nef.py`: the original Python prototype of `--dng` / `restore`
  (needs `pip install imagecodecs`).
* `nefcomp decode | skeleton | assemble`: low-level helpers.
* `tools/wicdecode.cpp`: note that the Nikon WIC codec only returns the embedded preview, so it is
  useless as a raw oracle.

## Credits / licences

* The HE encoder's transform & packet syntax are adapted from the forward model in
  [WonderLantern z8-qemu](https://github.com/WonderLantern/z8-qemu) (GPL-2.0-or-later).
* `third_party/nikon-he-public`: public HE decoder from LibRaw PR #826 (Dmitri Sotnikov, with
  z8-qemu fixes), LGPL-2.1 / CDDL. Used for `--verify` and for decoding HE sources.
* `third_party/libjxl`: libjxl 0.11.1 (BSD-3-Clause, with its bundled highway / brotli / skcms),
  built with JPEG transcoding. libjpeg-turbo (MSYS2) for regenerated previews.
* Nikon lossless/uncompressed formats follow dcraw / LibRaw.
* This program is therefore GPL-2.0-or-later. Nikon, Adobe and intoPIX own their trademarks and
  patents. This is an independent interoperability tool for your own files.

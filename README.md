# nefcomp

Shrinks Nikon NEF raw files after shooting:

* **lossless NEF → High Efficiency (HE) / High Efficiency★ (HE★) NEF**, exactly the format the
  camera writes. For cameras with HE (verified on all of them: Z 8, Z 9, Z f, Z6III, Z50II, Z5II, ZR).
* **any NEF → JPEG XL raw DNG** for cameras without HE (e.g. Z 6). The DNG keeps the raw mosaic
  and carries the original NEF's metadata, so `nefcomp restore` rebuilds a genuine Nikon NEF from it.

Originals are never modified: outputs are new files (written as `.part`, renamed when complete).

## Download

Prebuilt binaries are on the [releases page](https://github.com/missingno7/nefcomp/releases):
one self-contained command-line program per platform, no installation. Unpack and run it from a
terminal (`nefcomp --help`).

| platform | archive | notes |
| --- | --- | --- |
| Windows 10/11 x64 | `nefcomp-<version>-windows-x64.zip` | `nefcomp.exe` |
| Linux x64 | `nefcomp-<version>-linux-x64.tar.gz` | static binary, any distribution |
| macOS 11+ (Apple silicon and Intel) | `nefcomp-<version>-macos-universal.tar.gz` | not notarized: `xattr -d com.apple.quarantine nefcomp` once, or allow it in System Settings → Privacy & Security |

* **HE NEF conversion, `restore` and `info` work on all three.**
* **`--dng` needs [Adobe DNG Converter](https://helpx.adobe.com/camera-raw/using/adobe-dng-converter.html)**
  (free), which exists for Windows and macOS only. On Linux, make the DNGs on Windows/macOS;
  restoring them works everywhere. DNGs and restores are identical across platforms.
* Check a download against `SHA256SUMS` from the same release.

```
nefcomp [options] <file.NEF | dir> ...          lossless NEF -> HE / HE* NEF; HE* -> HE (-m he)
nefcomp --dng [options] <file.NEF | dir> ...    NEF -> JPEG XL raw DNG (restorable)
nefcomp --auto [options] <file.NEF | dir> ...   per file: HE NEF where possible, DNG otherwise
nefcomp restore [options] <file.dng | dir> ...  DNG made by --dng -> genuine Nikon NEF (lossless by default)
nefcomp info <file.NEF> ...
nefcomp --version

  -r, --recursive        include subfolders (with -o, the folder structure is mirrored)
  -m, --mode MODE        hestar (default) | he | lossless (lossless: --dng / --auto only)
      --dng              write a JPEG XL DNG instead of an HE NEF (needs Adobe DNG Converter)
  DNG options:
  -q, --quality Q        1..100: 100 = lossless, 70 = hestar (default), 55 = he
      --bpp B            instead of -q: target size, B bits per raw pixel on average
                         (not bit depth; data stays 14-bit). HE* = 5, HE = 3
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
      --force            write a DNG even if it is not smaller than the original NEF
      --as FORMAT        restore: lossless (default) | original
      --allow-untested   allow HE NEF output for HE cameras not yet verified
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

| mode | lossless NEF, verified HE camera | lossless / uncompressed / lossy NEF, other camera | HE★ NEF | HE NEF | |
| --- | --- | --- | --- | --- | --- |
| default (`-m hestar`) | HE★ NEF | skip (hint: `--auto` / `--dng`) | skip | skip (HE★ would be larger) | |
| `-m he` | HE NEF | skip | **HE NEF** (re-encoded) | skip | |
| `--dng` | DNG | DNG | DNG | DNG | |
| `--auto` | HE★ NEF (HE with `-m he`) | DNG | skip (HE with `-m he`) | skip | |

* **Not smaller:** a DNG that would not be smaller than its NEF (e.g. an HE file at the default q70)
  is skipped with its sizes shown. Lower `-q` / `--bpp`, or use `--force` to write it anyway.
* **Lossy NEFs** (Nikon "Compressed", e.g. Z 6 DSC_1207) go to DNG. nefcomp decodes the raw codes
  and uses Adobe's own curve for the linear values, so the DNG matches what Camera Raw shows for
  the NEF. The curve is stored in the embedded data. `restore` maps pixels to the nearest code (adding
  ~1.1 DN rms) and re-encodes with Nikon's lossy Huffman tree. The encoder reproduces the camera
  file byte-for-byte, and `-m lossless --keep-previews` restores it byte-identically. DSC_1207:
  25.7 MB → 13.7 MB at `-q 70`, 6.1 MB at `-q 45`.
* **HE★ → HE:** the HE★ data is decoded and re-encoded at 3 bpp, a second lossy generation (rms 9–18 DN
  over the HE★ pixels on the samples). The container is patched like a camera HE file; Adobe decodes
  it bit-identically.

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
  skip ...\102D4S\FU9_3802.NEF: unsupported raw format (small-size NEF (no raw mosaic))
done: 4 converted (158.2 MB -> 75.2 MB, saved 83.0 MB = 52%), 5 skipped, 0 failed
  skipped 4 x already High Efficiency
  skipped 1 x unsupported raw format
```

`--auto` writes HE NEFs for the verified models, which are all cameras with an HE mode. Future
HE bodies get DNGs until verified, unless `--allow-untested` is given. Files run one after another, each using all cores.

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

**bpp** = compressed raw-data bits ÷ sensor pixels (metadata and previews excluded). It's a size
target, not bit depth: the data stays 14-bit. `-q`, `--bpp`, `--distance` and `-m lossless` are
alternatives; nefcomp rejects combinations. 1 bpp is
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

### Tested cameras and formats

Every file below passes the regression (`tools/regress.py`): Adobe DNG Converter decodes nefcomp's
DNG; an exact (`-m lossless --keep-previews`) DNG restores a **byte-identical** NEF; the default
lossless restore has raw data equal to the DNG and opens in Adobe. Own files plus CC0 samples from
[raw.pixls.us](https://raw.pixls.us), at least one per raw format family.

| raw format | cameras tested |
| --- | --- |
| HE★ / HE (14-bit) | Z 8, Z 9, Z f, Z6III, Z50II (5600 px), Z5II, ZR |
| lossless 14-bit | Z 8, Z 7, Z 6, Z f, Z 9, Z6III, Z5II, D850, D800, D700, D7000, D5100, D500, D6, D3 |
| lossless 12-bit | D6, Z 6, Z 50 (data after the strip) |
| lossy 14-bit | Z 6, D6, D3, D780 (0x4440), Coolpix A |
| lossy 12-bit | D6 (0x4440), Z 6, 1 J2, 1 V1, D90 |
| lossy with "split" rows | Z 6II, D7000, D5000, D5, D5600, D3400, 1 J5 |
| lossy type 1 (0x4410 table, split) | D2H, D40, D80 (odd Adobe crop) |
| lossy, D1X (table in makernote 0x008C, bare-IFD makernote) | D1X |
| uncompressed 14-bit packed | Z 6, D6 |
| uncompressed 12-bit packed (LE, plain / row-padded) | Z 6, D4S |
| uncompressed 16-bit words (TIFF compression 1) | D3, D4S, D300 |
| uncompressed 12-bit MSB, multi-strip | D1, D200 |
| uncompressed 12-bit with pad byte (D100) | D100 |
| Coolpix, 12-bit in 32-bit words | P1000, P950, B700; P6000, P7000, P7100 (strip padded to 4096 bytes) |
| Coolpix, 12-bit MSB / 16-bit words | P340; P7700 (value << 4, kept at Adobe's 16-bit scale); A1000 (TIFF compression 1) |
| Coolpix E, 12-bit MSB interlaced (even rows, then odd rows) | E5400, E8400, E8800 |

What makes this general:
* **Format detection** follows LibRaw: TIFF compression, byte count vs. size, makernote 0x0051/0x0093
  compression code and the 0x0096 table version.
* **Encoders:** Nikon's lossless and lossy Huffman encoders are reproduced exactly (the camera pads the
  last byte with 1-bits). Lossy "split" streams switch to a coarser tree from a given row on; there the
  camera quantizes differences and an overshoot below zero is stored as zero (Adobe's reading; LibRaw
  keeps the negative value and drifts by a few codes). nefcomp encodes with the same closed loop.
* **Exact extras** (kept in exact DNGs): codes the lossy curve maps to the same value (ties), padding
  the camera wrote after the raw stream, and strips followed by other data (the strip slot is kept).
* **Adobe crop:** when Adobe crops the raw, nefcomp finds the crop by exact matching. The DNG stores Adobe's
  region, the cropped-off border goes losslessly into the embedded data, and `restore` puts it back.
* **Black level:** the companding curve uses Adobe's black level (0 on the D7000, 1008 on Z bodies).
* **Odd crops:** the Bayer plane split needs even sizes, so an odd Adobe crop (D80: 2611 rows) takes
  one more NEF row into the DNG and an ActiveArea keeps Adobe's region.
* **16-bit Coolpix files** (P7700): Adobe keeps the 16-bit word scale; nefcomp's LinearizationTable
  outputs the same scale.
* **Restore format:** HE/HE★ and lossy files from cameras that shoot lossless come back as lossless
  NEFs (12- or 14-bit, with Nikon's table in the makernote's byte order); lossy files from bodies
  without a lossless mode (D40…D5600, Nikon 1, Coolpix) and uncompressed files keep their own format.

Not supported (skipped with a reason): small-size NEFs (sRAW, YCbCr), Coolpix bodies with CMYG
sensors (E5700, …). Files whose raw data
Adobe decodes differently from nefcomp are refused when the DNG is made, never written.

## Build

C++17 with GCC or Clang, one `make`. Dependencies are static libjxl 0.11.1 and libjpeg-turbo:

```
sh third_party/build_libjxl.sh  .work/jxl  .deps/jxl     # needs git, CMake (Ninja optional)
sh third_party/build_libjpeg.sh .work/jpeg .deps/jpeg
make JXL=$PWD/.deps/jxl JPEG_LIB=$PWD/.deps/jpeg/lib/libjpeg.a JPEG_CFLAGS=-I$PWD/.deps/jpeg/include
```

* **Windows:** MSYS2 MinGW-w64 (`mingw32-make` or MSYS `make`) gives a static `nefcomp.exe`.
  `third_party/libjxl` holds a prebuilt MinGW libjxl, so plain `mingw32-make` works with the
  MSYS2 `libjpeg-turbo` package.
* **Linux:** a fully static `nefcomp`. **macOS:** set `CC=clang CXX=clang++`; third-party
  libraries are linked statically.
* `.github/workflows/build.yml` builds all three, runs `tools/ci_smoke.sh` (a public raw.pixls.us
  sample must convert to the same HE★ bytes on every platform) and packages them with
  `tools/package_release.sh`. A `v*` tag creates a draft release.

`--dng` needs Adobe DNG Converter installed (tested with 17.2 and 19.0); it is used to produce
Adobe's colour profile and metadata.

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

**Verified cameras:**

| camera | how |
| --- | --- |
| Z 8 | container derived from back-to-back lossless / HE / HE★ shots; Camera Raw |
| Z 9, Z f (FW 1.00) | lossless (raw.pixls.us) → HE / HE★ matches the camera's own HE / HE★ files in every format field; Adobe decodes identically |
| Z50II (FW 1.00), ZR (FW 1.00.00, 1.01) | camera HE★ (Photography Blog) → lossless → HE★ reproduces every container field of the camera file; Adobe decodes identically |
| Z6III, Z5II (FW 1.00) | lossless (Photography Blog) → HE / HE★: same field changes and header as the verified bodies; Adobe decodes identically (no camera HE sample found) |

"Every format field": codestream header (155 bytes), strip size, raw-IFD and makernote tag layout,
0xC7D5 items 0x000D/0x000E/0x000F and makernote 0x0051/0x0058. Per-shot items (0xC7D5
0x0006–0x0009, 0x0010) differ between shots, as they do between the camera's own files. Error vs.
the lossless source: HE★ 2.6–3.5 DN rms, HE 8.7–10.4 DN.

The Z50II exposed a bug shared by the bundled decoder and nefcomp's encoder: each line block's
significance substream is `ceil(Σ ceil(ng/8) / 8)` bytes over its sub-bands, not
`ceil(width/2/256)`. The two agree for the Z 8, Z 9, Z f and Z6III widths, but at 5600 px three
line blocks need 12 bytes instead of 11, so Z50II files didn't decode before.

Checking the Z5II needs an Adobe DNG Converter newer than 17.2 (verified with 19.0).

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

`nefcomp restore` decodes the tiles and writes a **lossless NEF** by default for HE★, HE and lossy
originals (lossy only from cameras that shoot lossless themselves; uncompressed originals stay
uncompressed, which is lossless too). That's the failsafe: no loss beyond what the DNG already
has, and the file can be edited in camera and opened in NX Studio. The container gets the camera's
lossless metadata (mode 3, zeroed size fields, Nikon's lossless table in makernote 0x0096 and
0xC7D5/0x000E, which is identical on every Z 8 / Z 6 lossless file examined). Where the table has to be
added, the makernote is moved to the end of the metadata (its offsets are self-relative), with a new
IFD. Restored lossless NEFs from HE★, HE, lossy and uncompressed sources have the same tag structure as
a camera lossless file, raw data identical to the DNG, and Adobe decodes them identically.
`--as original` re-encodes in the original format instead (smaller, but another lossy pass for
HE/lossy sources). It is the mode that gives byte-identical files from `-m lossless --keep-previews`
DNGs of non-lossless originals; lossless originals come back byte-identical in either mode. Nikon's lossless encoder is reproduced exactly:
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
  built with JPEG transcoding. libjpeg-turbo for regenerated previews. Licence texts of everything
  linked into the binaries: `THIRD_PARTY_NOTICES.md` (and `licenses/` in release archives).
* Nikon lossless/uncompressed formats follow dcraw / LibRaw.
* This program is therefore GPL-2.0-or-later. Nikon, Adobe and intoPIX own their trademarks and
  patents. This is an independent interoperability tool for your own files.

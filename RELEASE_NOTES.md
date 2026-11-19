First public release of **nefcomp**, a command-line tool that shrinks Nikon raw files after shooting.

### What it does

* **Lossless NEF → High Efficiency★ / High Efficiency NEF**: the same format the camera writes,
  for every Nikon camera with an HE mode (Z 8, Z 9, Z f, Z6III, Z50II, Z5II, ZR, all verified
  against camera files and Adobe). HE★ files can also be re-encoded to HE.
* **Any NEF → JPEG XL raw DNG** (`--dng`, needs Adobe DNG Converter on Windows or macOS): for
  cameras without HE, from the D1 to today's Z bodies and Coolpix NRW. The DNG carries
  everything needed to rebuild the NEF.
* **`restore`**: DNG → genuine Nikon NEF (lossless by default; `--as original` keeps the original
  format). `-m lossless --keep-previews` DNGs restore byte-identical NEFs.
* Batch mode for whole cards (`--auto -r -o …`), originals are never modified.

### Downloads

| platform | file |
| --- | --- |
| Windows 10/11 x64 | `nefcomp-*-windows-x64.zip` |
| Linux x64 (static) | `nefcomp-*-linux-x64.tar.gz` |
| macOS 11+ universal (Apple silicon + Intel) | `nefcomp-*-macos-universal.tar.gz` |

The macOS binary is not notarized: run `xattr -d com.apple.quarantine nefcomp` once (or allow it
in System Settings → Privacy & Security). Verify downloads with `SHA256SUMS`.

### Known limitations

* `--dng` is unavailable on Linux (no Adobe DNG Converter); HE conversion, `restore` and `info` work.
* Not supported: small-size NEFs (sRAW) and Coolpix models with CMYG sensors (E5700 …).
* Regenerated previews (default DNG mode) do not reproduce the camera's Picture Control look;
  use `--keep-previews` to keep the originals.

See the README for formats, measurements and how each camera was verified.

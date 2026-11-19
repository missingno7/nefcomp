# Third-party software in nefcomp binaries

nefcomp is licensed under the GNU General Public License, version 2 or later (see `COPYING`).
Its release binaries are statically linked with the libraries below. Their licence texts are in
the `licenses/` folder of each release archive and in `third_party/` of the source tree.

| component | version | licence | used for |
| --- | --- | --- | --- |
| [libjxl](https://github.com/libjxl/libjxl) | 0.11.1 | BSD-3-Clause + patent grant (`libjxl-LICENSE`, `libjxl-PATENTS`) | JPEG XL raw data in DNGs |
| [highway](https://github.com/google/highway) (bundled with libjxl) | | Apache-2.0 / BSD-3-Clause (`highway-LICENSE`, `highway-LICENSE-BSD3`) | SIMD for libjxl |
| [brotli](https://github.com/google/brotli) (bundled with libjxl) | | MIT (`brotli-LICENSE`) | compression of the embedded NEF data |
| [skcms](https://skia.googlesource.com/skcms) (bundled with libjxl) | | BSD-3-Clause (`skcms-LICENSE`) | colour management in libjxl |
| [libjpeg-turbo](https://libjpeg-turbo.org) | 3.0.x | IJG + BSD-3-Clause + zlib (`libjpeg-turbo-LICENSE.md`, `libjpeg-turbo-README.ijg`) | regenerated NEF preview JPEGs |
| [zlib](https://zlib.net) (Windows and Linux builds; macOS uses the system copy) | | zlib (`zlib-copyright.txt`) | reading version-1 embedded data |
| Nikon HE public decoder (`third_party/nikon-he-public`, from LibRaw PR #826 by Dmitri Sotnikov, with WonderLantern z8-qemu fixes) | | LGPL-2.1 or CDDL-1.0 (`nikon-he-public-LGPL-2.1.txt`, `nikon-he-public-CDDL-1.0.txt`) | decoding HE / HE★ NEFs |

The HE encoder's transform and packet syntax are adapted from the forward model in
[WonderLantern z8-qemu](https://github.com/WonderLantern/z8-qemu) (GPL-2.0-or-later).
Nikon lossless, lossy and uncompressed raw formats follow the public dcraw / LibRaw descriptions.

The Windows and Linux binaries also contain the GCC runtime (libstdc++, libgcc; GCC Runtime Library
Exception) and, on Windows, the MinGW-w64 runtime.

Nikon, Adobe and intoPIX own their trademarks and patents. nefcomp is an independent
interoperability tool for your own files and is not affiliated with or endorsed by them.

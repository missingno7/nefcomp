#!/bin/sh
# Build a static libjxl (headers + lib/*.a: jxl, jxl_cms, hwy, brotli) for nefcomp.
# Works with MinGW-w64 (MSYS2), Linux g++ and macOS clang. Needs git, CMake
# (and Ninja if available).
#
# usage: sh third_party/build_libjxl.sh [workdir] [prefix]
#   prefix defaults to third_party/libjxl (what the Makefile uses by default)
# environment: CC / CXX (default gcc / g++), CMAKE_EXTRA (e.g.
#   -DCMAKE_OSX_ARCHITECTURES=x86_64), BUILD_DIR (default build)
set -e
here=$(cd "$(dirname "$0")" && pwd)
work=${1:-$here/../.libjxl-build}
dst=${2:-$here/libjxl}
bdir=${BUILD_DIR:-build}
mkdir -p "$work"
cd "$work"
[ -d libjxl ] || git clone --depth 1 --branch v0.11.1 https://github.com/libjxl/libjxl
cd libjxl
git submodule update --init --depth 1 third_party/highway third_party/brotli third_party/skcms
gen=""
command -v ninja >/dev/null 2>&1 && gen="-G Ninja"
# shellcheck disable=SC2086
cmake -S . -B "$bdir" $gen -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_C_COMPILER="${CC:-gcc}" -DCMAKE_CXX_COMPILER="${CXX:-g++}" \
  -DBUILD_SHARED_LIBS=OFF -DBUILD_TESTING=OFF -DJPEGXL_STATIC=ON \
  -DJPEGXL_ENABLE_TOOLS=OFF -DJPEGXL_ENABLE_DOXYGEN=OFF -DJPEGXL_ENABLE_MANPAGES=OFF \
  -DJPEGXL_ENABLE_BENCHMARK=OFF -DJPEGXL_ENABLE_EXAMPLES=OFF -DJPEGXL_ENABLE_JPEGLI=OFF \
  -DJPEGXL_ENABLE_SJPEG=OFF -DJPEGXL_ENABLE_OPENEXR=OFF -DJPEGXL_ENABLE_JNI=OFF \
  -DJPEGXL_ENABLE_PLUGINS=OFF -DJPEGXL_ENABLE_SKCMS=ON -DJPEGXL_ENABLE_TRANSCODE_JPEG=ON \
  -DJPEGXL_ENABLE_VIEWERS=OFF -DJPEGXL_ENABLE_FUZZERS=OFF -DJPEGXL_ENABLE_DEVTOOLS=OFF \
  -DJPEGXL_BUNDLE_LIBPNG=OFF -DJPEGXL_FORCE_SYSTEM_BROTLI=OFF -DJPEGXL_FORCE_SYSTEM_HWY=OFF \
  $CMAKE_EXTRA
cmake --build "$bdir" --target jxl jxl_cms --parallel
mkdir -p "$dst/include/jxl" "$dst/include/brotli" "$dst/lib"
cp lib/include/jxl/*.h "$dst/include/jxl/"
cp "$bdir"/lib/include/jxl/*.h "$dst/include/jxl/"  # generated headers (version, export)
cp third_party/brotli/c/include/brotli/*.h "$dst/include/brotli/"
cp "$bdir"/lib/libjxl.a "$bdir"/lib/libjxl_cms.a "$bdir"/third_party/highway/libhwy.a \
   "$bdir"/third_party/brotli/libbrotli*.a "$dst/lib/"
cp LICENSE PATENTS "$dst/"
cp third_party/brotli/LICENSE "$dst/LICENSE.brotli"
cp third_party/highway/LICENSE "$dst/LICENSE.highway"
cp third_party/skcms/LICENSE "$dst/LICENSE.skcms"
echo "libjxl installed into $dst"

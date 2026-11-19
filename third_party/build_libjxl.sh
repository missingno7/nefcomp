#!/bin/sh
# Rebuild the static libjxl in third_party/libjxl (headers + lib/*.a) with MinGW-w64 g++.
# Needs git, CMake and Ninja (e.g. the copies bundled with Visual Studio), and
# MSYS2 mingw64 gcc/g++ on PATH. Usage: sh third_party/build_libjxl.sh [workdir]
set -e
here=$(cd "$(dirname "$0")" && pwd)
work=${1:-$here/../.libjxl-build}
mkdir -p "$work"
cd "$work"
[ -d libjxl ] || git clone --depth 1 --branch v0.11.1 https://github.com/libjxl/libjxl
cd libjxl
git submodule update --init --depth 1 third_party/highway third_party/brotli third_party/skcms
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_C_COMPILER=gcc -DCMAKE_CXX_COMPILER=g++ \
  -DBUILD_SHARED_LIBS=OFF -DBUILD_TESTING=OFF -DJPEGXL_STATIC=ON \
  -DJPEGXL_ENABLE_TOOLS=OFF -DJPEGXL_ENABLE_DOXYGEN=OFF -DJPEGXL_ENABLE_MANPAGES=OFF \
  -DJPEGXL_ENABLE_BENCHMARK=OFF -DJPEGXL_ENABLE_EXAMPLES=OFF -DJPEGXL_ENABLE_JPEGLI=OFF \
  -DJPEGXL_ENABLE_SJPEG=OFF -DJPEGXL_ENABLE_OPENEXR=OFF -DJPEGXL_ENABLE_JNI=OFF \
  -DJPEGXL_ENABLE_PLUGINS=OFF -DJPEGXL_ENABLE_SKCMS=ON -DJPEGXL_ENABLE_TRANSCODE_JPEG=ON \
  -DJPEGXL_ENABLE_VIEWERS=OFF -DJPEGXL_ENABLE_FUZZERS=OFF -DJPEGXL_ENABLE_DEVTOOLS=OFF \
  -DJPEGXL_BUNDLE_LIBPNG=OFF -DJPEGXL_FORCE_SYSTEM_BROTLI=OFF -DJPEGXL_FORCE_SYSTEM_HWY=OFF
cmake --build build --target jxl jxl_cms
dst="$here/libjxl"
mkdir -p "$dst/include/jxl" "$dst/include/brotli" "$dst/lib"
cp lib/include/jxl/*.h build/lib/include/jxl/*.h "$dst/include/jxl/"
cp third_party/brotli/c/include/brotli/*.h "$dst/include/brotli/"
cp build/lib/libjxl.a build/lib/libjxl_cms.a build/third_party/highway/libhwy.a \
   build/third_party/brotli/libbrotli*.a "$dst/lib/"
cp LICENSE PATENTS "$dst/"
cp third_party/brotli/LICENSE "$dst/LICENSE.brotli"
echo "libjxl installed into $dst"

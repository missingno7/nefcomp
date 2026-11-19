#!/bin/sh
# Build a static libjpeg-turbo (include/ + lib/libjpeg.a) for nefcomp's preview
# JPEGs, where no static system libjpeg is available (macOS, minimal Linux).
#
# usage: sh third_party/build_libjpeg.sh [workdir] [prefix]
# environment: CC (default cc), CMAKE_EXTRA (e.g. -DCMAKE_OSX_ARCHITECTURES=x86_64),
#   BUILD_DIR (default build)
set -e
work=${1:-.libjpeg-build}
dst=${2:-$work/install}
bdir=${BUILD_DIR:-build}
mkdir -p "$work"
cd "$work"
[ -d libjpeg-turbo ] || git clone --depth 1 --branch 3.0.4 https://github.com/libjpeg-turbo/libjpeg-turbo
cd libjpeg-turbo
gen=""
command -v ninja >/dev/null 2>&1 && gen="-G Ninja"
# shellcheck disable=SC2086
cmake -S . -B "$bdir" $gen -DCMAKE_BUILD_TYPE=Release -DCMAKE_C_COMPILER="${CC:-cc}" \
  -DENABLE_SHARED=OFF -DENABLE_STATIC=ON -DWITH_TURBOJPEG=OFF -DWITH_JAVA=OFF \
  -DCMAKE_INSTALL_PREFIX="$dst" -DCMAKE_INSTALL_LIBDIR=lib $CMAKE_EXTRA
cmake --build "$bdir" --parallel
cmake --install "$bdir"
cp LICENSE.md "$dst/LICENSE.libjpeg-turbo.md"
echo "libjpeg-turbo installed into $dst"

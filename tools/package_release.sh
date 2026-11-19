#!/bin/sh
# Package a nefcomp binary with its documentation and licences.
# usage: sh tools/package_release.sh <binary> <platform> <version> [outdir]
#   creates <outdir>/nefcomp-<version>-<platform>.zip (Windows) or .tar.gz
set -e
bin=$1
platform=$2
version=$3
out=${4:-dist}
[ -f "$bin" ] && [ -n "$platform" ] && [ -n "$version" ] || { echo "usage: $0 <binary> <platform> <version> [outdir]" >&2; exit 2; }
name="nefcomp-$version-$platform"
stage="$out/$name"
rm -rf "$stage"
mkdir -p "$stage/licenses"
cp "$bin" "$stage/"
cp README.md COPYING THIRD_PARTY_NOTICES.md "$stage/"
L="$stage/licenses"
cp third_party/libjxl/LICENSE "$L/libjxl-LICENSE"
cp third_party/libjxl/PATENTS "$L/libjxl-PATENTS"
cp third_party/libjxl/LICENSE.brotli "$L/brotli-LICENSE"
cp third_party/libjxl/LICENSE.highway "$L/highway-LICENSE"
cp third_party/libjxl/LICENSE.highway-BSD3 "$L/highway-LICENSE-BSD3"
cp third_party/libjxl/LICENSE.skcms "$L/skcms-LICENSE"
cp third_party/licenses/libjpeg-turbo-LICENSE.md third_party/licenses/libjpeg-turbo-README.ijg "$L/"
cp third_party/licenses/zlib-copyright.txt "$L/"
cp third_party/nikon-he-public/LICENSE.LGPL "$L/nikon-he-public-LGPL-2.1.txt"
cp third_party/nikon-he-public/LICENSE.CDDL "$L/nikon-he-public-CDDL-1.0.txt"
cd "$out"
case "$platform" in
  windows*) rm -f "$name.zip"; (cd "$name" && zip -qr "../$name.zip" .) ; echo "$out/$name.zip" ;;
  *) rm -f "$name.tar.gz"; tar -czf "$name.tar.gz" "$name"; echo "$out/$name.tar.gz" ;;
esac

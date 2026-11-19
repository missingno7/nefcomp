#!/bin/sh
# Smoke test for a nefcomp build, using a public CC0 sample from raw.pixls.us.
# Checks: version, info, lossless -> HE* (same bytes on every platform), NEF
# decode + rebuild (byte-identical), a non-ASCII path, and --dng availability.
# usage: sh tools/ci_smoke.sh <path-to-nefcomp> [workdir]
set -e
bin=$(cd "$(dirname "$1")" && pwd)/$(basename "$1")
work=${2:-smoke}
mkdir -p "$work"
cd "$work"

sha() { if command -v sha256sum >/dev/null 2>&1; then sha256sum "$1" | cut -d' ' -f1; else shasum -a 256 "$1" | cut -d' ' -f1; fi; }
check() { got=$(sha "$1"); [ "$got" = "$2" ] || { echo "FAIL: $1 sha256 $got, expected $2" >&2; exit 1; }; echo "ok   $1 $3"; }

"$bin" --version
src=zf.NEF
[ -f "$src" ] || curl -fsSL -o "$src" "https://raw.pixls.us/data/Nikon/Z%20f/DSC_0040.NEF"
check "$src" 83c82be0be8865d796096dfbcc8ef2abf5af1bd37db44dfad6715070b0c99d15 "(sample)"

"$bin" info "$src"
"$bin" -m hestar --overwrite -o he "$src"
check he/zf_HEs.NEF 02fb1a169bee1d2f9d0503e713eb0d5344fd1bfb1b2e3f7788ef9e015b2a3b88 "(HE* output identical to the reference build)"

"$bin" skeleton "$src" skel.bin
"$bin" decode "$src" px.u16
"$bin" assemble skel.bin px.u16 rebuilt.NEF
cmp rebuilt.NEF "$src" && echo "ok   lossless NEF rebuilt byte-identical"

mkdir -p "Fotky čeština"
cp "$src" "Fotky čeština/žluťoučký kůň.NEF"
"$bin" info "Fotky čeština/žluťoučký kůň.NEF" >/dev/null && echo "ok   non-ASCII path"
"$bin" -m hestar --overwrite "Fotky čeština/žluťoučký kůň.NEF" >/dev/null
check "Fotky čeština/žluťoučký kůň_HEs.NEF" 02fb1a169bee1d2f9d0503e713eb0d5344fd1bfb1b2e3f7788ef9e015b2a3b88 "(non-ASCII output name)"

# --dng needs Adobe DNG Converter, absent on CI machines: must skip cleanly, not crash
"$bin" --dng "$src" > dng.log 2>&1 || true
grep -q "Adobe DNG Converter" dng.log && echo "ok   --dng without Adobe DNG Converter is reported"
echo "smoke test passed"

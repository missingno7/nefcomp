#!/bin/sh
# Try converting each given NEF with Adobe DNG Converter; print OK/FAIL per file.
# usage: dngtest.sh outdir file.NEF ...
DNGC="/c/Program Files/Adobe/Adobe DNG Converter/Adobe DNG Converter.exe"
out="$1"; shift
mkdir -p "$out"
for f in "$@"; do
    b=$(basename "${f%.*}")
    rm -f "$out/$b.dng"
    "$DNGC" -c -u -d "$(cygpath -w "$out")" "$(cygpath -w "$f")" >/dev/null 2>&1
    if [ -f "$out/$b.dng" ]; then echo "OK   $b"; else echo "FAIL $b"; fi
done

#!/bin/sh
# Engine write throughput on a 2 GB image for several chunk sizes (no sanitizers, -O2).
set -eu
ROOT=$(CDPATH= cd -- "$(dirname "$0")/.." && pwd)
V="$ROOT/vendor/build"
WORK=$(mktemp -d -t xntfs-bench); trap 'rm -rf "$WORK"' EXIT
clang -O2 -I"$ROOT/Sources/Module/Bridge" -I"$V/include" -I"$V/include/ntfs-3g" \
  "$ROOT"/Sources/Module/Bridge/*.c "${1:?bench.c}" "$V/lib/libntfs-3g.a" -framework CoreFoundation -o "$WORK/bench"
for c in 131072 1048576 8388608; do
  mkfile -n 2g "$WORK/img"; "$V/bin/mkntfs" -q -F -f "$WORK/img" 2>/dev/null
  "$WORK/bench" "$WORK/img" $c
done

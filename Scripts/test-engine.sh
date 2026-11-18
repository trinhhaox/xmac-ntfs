#!/bin/sh
# Builds the C engine with the vendored libntfs-3g and runs it against a fresh NTFS image,
# then has ntfsprogs check the result independently.
set -eu
ROOT=$(CDPATH= cd -- "$(dirname "$0")/.." && pwd)
V="$ROOT/vendor/build"
[ -f "$V/lib/libntfs-3g.a" ] || "$ROOT/Scripts/build-vendor.sh"
WORK=$(mktemp -d -t xntfs-test)
trap 'rm -rf "$WORK"' EXIT

clang -std=gnu11 -O1 -g -Wall -Wextra -Wno-unused-parameter -fsanitize=address,undefined \
  -I"$ROOT/Sources/Module/Bridge" -I"$V/include" -I"$V/include/ntfs-3g" \
  "$ROOT"/Sources/Module/Bridge/*.c "$ROOT/Tests/engine/engine_test.c" "$V/lib/libntfs-3g.a" -framework CoreFoundation \
  -o "$WORK/engine_test"

clang -std=gnu11 -O1 -g -Wall -Wextra -Wno-unused-parameter -fsanitize=address,undefined \
  -I"$ROOT/Sources/Module/Bridge" -I"$V/include" -I"$V/include/ntfs-3g" \
  "$ROOT"/Sources/Module/Bridge/*.c "$ROOT/Tests/engine/fuzz_test.c" "$V/lib/libntfs-3g.a" -framework CoreFoundation \
  -o "$WORK/fuzz_test"

mkfile -n 64m "$WORK/img.ntfs"
"$V/bin/mkntfs" -q -F -f -L XTEST "$WORK/img.ntfs" 2>/dev/null
"$WORK/engine_test" "$WORK/img.ntfs"

for SEED in ${SEEDS:-1 7 42}; do
  mkfile -n 64m "$WORK/fuzz.ntfs"
  "$V/bin/mkntfs" -q -F -f -L FUZZ "$WORK/fuzz.ntfs" 2>/dev/null
  "$WORK/fuzz_test" "$WORK/fuzz.ntfs" "$SEED"
  "$V/bin/ntfsfix" -n "$WORK/fuzz.ntfs" | tail -1
done

echo "--- independent check (ntfsprogs)"
"$V/bin/ntfsls" "$WORK/img.ntfs"
"$V/bin/ntfsls" "$WORK/img.ntfs" -p "/Đổi tên"
"$V/bin/ntfsfix" -n "$WORK/img.ntfs" | tail -3

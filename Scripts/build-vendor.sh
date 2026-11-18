#!/bin/sh
# Builds static libntfs-3g (GPL-2.0-or-later, tuxera/ntfs-3g) for the module, plus ntfsprogs
# (mkntfs, ntfsls, ntfscat, ntfsfix) used by the engine tests. No FUSE driver.
set -eu
ROOT=$(CDPATH= cd -- "$(dirname "$0")/.." && pwd)
SRC="$ROOT/vendor/ntfs-3g"
COMMIT=7f0f841fc52cf719106c5c93bafe465004e36816
ARCHS=${ARCHS:-"arm64 x86_64"}
export PATH="/opt/homebrew/opt/libtool/libexec/gnubin:$PATH"

if [ ! -d "$SRC/.git" ]; then
  git clone --quiet https://github.com/tuxera/ntfs-3g.git "$SRC"
fi
git -C "$SRC" checkout --quiet "$COMMIT"
cd "$SRC"
[ -x configure ] || ./autogen.sh >/dev/null

OUT="$ROOT/vendor/build"
rm -rf "$OUT" && mkdir -p "$OUT/lib" "$OUT/bin" "$OUT/include"
LIBS=""
for ARCH in $ARCHS; do
  make distclean >/dev/null 2>&1 || true
  ./configure --quiet --disable-ntfs-3g --disable-plugins --disable-shared --enable-static \
    --disable-dependency-tracking --host="$ARCH-apple-darwin" \
    CC=clang CFLAGS="-O2 -arch $ARCH -mmacosx-version-min=15.4" >/dev/null
  make -j"$(sysctl -n hw.ncpu)" >/dev/null
  cp libntfs-3g/.libs/libntfs-3g.a "$OUT/lib/libntfs-3g-$ARCH.a"
  LIBS="$LIBS $OUT/lib/libntfs-3g-$ARCH.a"
  if [ "$ARCH" = "$(uname -m)" ]; then
    for t in mkntfs ntfsls ntfscat ntfsfix ntfsinfo; do cp "ntfsprogs/$t" "$OUT/bin/"; done
    cp config.h "$OUT/include/"
  fi
done
lipo -create $LIBS -output "$OUT/lib/libntfs-3g.a"
cp -R include/ntfs-3g "$OUT/include/"
echo "OK: $OUT/lib/libntfs-3g.a ($ARCHS), tools in $OUT/bin"

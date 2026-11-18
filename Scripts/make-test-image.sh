#!/bin/sh
# Creates a fresh NTFS image and attaches it without mounting, for trying the FSKit module
# on a disk that holds nothing of value. Prints the /dev/diskN node.
# Usage: Scripts/make-test-image.sh [size, default 256m] [label, default XMACTEST]
set -eu
ROOT=$(CDPATH= cd -- "$(dirname "$0")/.." && pwd)
IMG_DIR="$ROOT/build/test-images"
IMG="$IMG_DIR/${2:-XMACTEST}.ntfs"
mkdir -p "$IMG_DIR"
rm -f "$IMG"
mkfile -n "${1:-256m}" "$IMG"
"$ROOT/vendor/build/bin/mkntfs" -q -F -f -L "${2:-XMACTEST}" "$IMG" 2>/dev/null
diskutil image attach --noMount "$IMG" | awk '{print $1; exit}'

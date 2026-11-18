#!/bin/sh
# Independent check of an NTFS image or device with ntfsprogs (no changes made).
# Usage: Scripts/check-image.sh <image-or-/dev/diskN> [path to list]
set -eu
ROOT=$(CDPATH= cd -- "$(dirname "$0")/.." && pwd)
BIN="$ROOT/vendor/build/bin"
"$BIN/ntfsfix" -n "$1" | tail -2
"$BIN/ntfsls" -l "$1" -p "${2:-/}"

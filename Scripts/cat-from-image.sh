#!/bin/sh
# Prints a file from an NTFS image with ntfsprogs (independent of the engine).
# Usage: Scripts/cat-from-image.sh <image> <path inside the volume>
set -eu
ROOT=$(CDPATH= cd -- "$(dirname "$0")/.." && pwd)
"$ROOT/vendor/build/bin/ntfscat" "$1" "$2"

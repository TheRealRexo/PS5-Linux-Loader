#!/bin/bash
# patch-initrd.sh - append the loop-mount /init (seg3.cpio) to an initrd.img
# so the PS5 can loop-mount a .img from M.2/USB at boot.
set -euo pipefail

INITRD="${1:?usage: patch-initrd.sh <initrd.img> [output]}"
OUT="${2:-$1}"

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
INIT_SCRIPT="$REPO_ROOT/initrd/init"

[ -f "$INITRD" ] || { echo "initrd not found: $INITRD" >&2; exit 1; }
[ -f "$INIT_SCRIPT" ] || { echo "init script not found: $INIT_SCRIPT" >&2; exit 1; }

WORK="$(mktemp -d)"
trap 'rm -rf "$WORK"' EXIT

mkdir -p "$WORK/initfs"
cp "$INIT_SCRIPT" "$WORK/initfs/init"
chmod +x "$WORK/initfs/init"

# Build seg3.cpio (newc) listing the "init" file.
( cd "$WORK/initfs" && printf "init\n" | cpio -o -H newc ) > "$WORK/seg3.cpio" 2>/dev/null

# Pad the original initrd to a 512-byte boundary, then append seg3.cpio.
cp "$INITRD" "$WORK/out.img"
size=$(wc -c < "$WORK/out.img" | tr -d " ")
pad=$(( (512 - (size % 512)) % 512 ))
if [ "$pad" -gt 0 ]; then
    dd if=/dev/zero of="$WORK/pad" bs=1 count="$pad" status=none
    cat "$WORK/pad" >> "$WORK/out.img"
fi
cat "$WORK/seg3.cpio" >> "$WORK/out.img"

cp "$WORK/out.img" "$OUT"
echo "patched: $OUT ($(wc -c < "$OUT" | tr -d " ") bytes)"

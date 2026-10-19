#!/bin/bash
# inject-os-image.sh - turn a stock ps5-linux image into a ps5-linux "OS image".
#
# Copies the M.2 / install tooling into the image's root filesystem and drops a
# sidecar descriptor next to the image so the loader's WebUI can recognise it.
#
# Must run on Linux (uses losetup -P). In CI it runs on the GitHub runner.
set -euo pipefail

IMG=""
MOUNT_DIR="/mnt/ps5_os_image"
TOOLS_DIR="$(cd "$(dirname "$0")" && pwd)"

usage() {
    echo "Usage: $0 --img <ps5-*.img>" >&2
    exit 1
}

while [ $# -gt 0 ]; do
    case "$1" in
        --img) IMG="${2:-}"; shift 2 ;;
        -h|--help) usage ;;
        *) echo "unknown argument: $1" >&2; usage ;;
    esac
done

[ -n "$IMG" ] || usage
[ -f "$IMG" ] || { echo "image not found: $IMG" >&2; exit 1; }
[ -x "$TOOLS_DIR/m2_init" ] || { echo "m2_init not built (run make in $TOOLS_DIR)" >&2; exit 1; }

LOOP=""
cleanup() {
    set +e
    mountpoint -q "$MOUNT_DIR" && umount "$MOUNT_DIR"
    [ -n "$LOOP" ] && losetup -d "$LOOP" 2>/dev/null
}
trap cleanup EXIT

echo ">> attaching $IMG"
LOOP="$(losetup -Pf --show "$IMG")"
for _ in $(seq 1 10); do [ -b "${LOOP}p1" ] && break; sleep 1; done

ROOT_PART=""
ROOT_LABEL=""
for p in "${LOOP}"p*; do
    [ -b "$p" ] || continue
    if [ "$(blkid -s TYPE -o value "$p" 2>/dev/null)" = "ext4" ]; then
        ROOT_PART="$p"
        ROOT_LABEL="$(blkid -s LABEL -o value "$p" 2>/dev/null || true)"
        break
    fi
done
[ -n "$ROOT_PART" ] || { echo "no ext4 root filesystem in $IMG" >&2; exit 1; }
echo ">> root filesystem: $ROOT_PART ($ROOT_LABEL)"

mkdir -p "$MOUNT_DIR"
mount "$ROOT_PART" "$MOUNT_DIR"

echo ">> installing tools"
install -d "$MOUNT_DIR/usr/local/sbin" "$MOUNT_DIR/usr/local/bin"
install -m 0755 "$TOOLS_DIR/m2_init"      "$MOUNT_DIR/usr/local/sbin/m2_init"
install -m 0755 "$TOOLS_DIR/ps5-osinstall" "$MOUNT_DIR/usr/local/bin/ps5-osinstall"
install -m 0755 "$TOOLS_DIR/ps5-osexec"    "$MOUNT_DIR/usr/local/bin/ps5-osexec"

echo ">> writing OS image marker"
cat > "$MOUNT_DIR/etc/ps5-os-image" <<EOF
name=$(basename "$IMG")
root_label=$ROOT_LABEL
built_utc=$(date -u +%Y-%m-%dT%H:%M:%SZ)
tooling=ps5-linux-tools
EOF

sync
umount "$MOUNT_DIR"

SIZE_BYTES="$(stat -c %s "$IMG")"
cat > "${IMG}.osimage" <<EOF
# ps5-linux OS image descriptor (read by the loader WebUI)
name=$(basename "$IMG")
root_label=$ROOT_LABEL
image_size_gb=$(( (SIZE_BYTES + 1073741823) / 1073741824 ))
tools=1
EOF

echo ">> done"
echo "   marker: /etc/ps5-os-image"
echo "   sidecar: ${IMG}.osimage"

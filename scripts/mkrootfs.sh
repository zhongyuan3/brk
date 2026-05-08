#!/usr/bin/env bash
# Build a brkfs root disk image for QEMU (see Makefile ROOTFS_IMG).
#
# Layout: three sibling git checkouts (override with env):
#   BRK_KERNEL_ROOT    this repository (default: parent of scripts/)
#   BRK_USER_ROOT      brk-user (default: $BRK_KERNEL_ROOT/../brk-user)
#   BRK_FSTOOLS_ROOT    brkfstools (default: $BRK_KERNEL_ROOT/../brkfstools)
#
# Optional:
#   BRK_ROOTFS_BYTES   raw image size in bytes (default: 4194304 = 4MiB, matches include/brk/dev.h DISK0_SIZE)
#   BRK_SKIP_USER_BUILD  if set, do not run make in brk-user
#   BRK_SKIP_FSTOOLS_BUILD  if set, do not run make in brkfstools
#   CROSS_COMPILE      forwarded to brk-user make when building userland
#
# Usage: mkrootfs.sh <path/to/rootfs.img>

set -euo pipefail

usage() {
	echo "usage: ${0##*/} <output-rootfs.img>" >&2
	exit 1
}

[[ $# -eq 1 ]] || usage
IMG=$1

BRK_KERNEL_ROOT=$(cd "$(dirname "$0")/.." && pwd)
BRK_USER_ROOT=${BRK_USER_ROOT:-"$BRK_KERNEL_ROOT/../brk-user"}
BRK_FSTOOLS_ROOT=${BRK_FSTOOLS_ROOT:-"$BRK_KERNEL_ROOT/../brkfstools"}
BRK_ROOTFS_BYTES=${BRK_ROOTFS_BYTES:-$((4096 * 1024))}

for d in "$BRK_USER_ROOT" "$BRK_FSTOOLS_ROOT"; do
	if [[ ! -d $d ]]; then
		echo "${0##*/}: missing directory: $d" >&2
		echo "Set BRK_USER_ROOT / BRK_FSTOOLS_ROOT if your layout differs." >&2
		exit 1
	fi
done

MKFS=$BRK_FSTOOLS_ROOT/mkfs.brkfs
CP=$BRK_FSTOOLS_ROOT/cp.brkfs
if [[ ! -x $MKFS ]] || [[ ! -x $CP ]]; then
	if [[ -n ${BRK_SKIP_FSTOOLS_BUILD:-} ]]; then
		echo "${0##*/}: $MKFS or $CP not found (BRK_SKIP_FSTOOLS_BUILD is set)" >&2
		exit 1
	fi
	make -C "$BRK_FSTOOLS_ROOT"
fi
[[ -x $MKFS && -x $CP ]] || {
	echo "${0##*/}: expected host tools: $MKFS $CP" >&2
	exit 1
}

if [[ -z ${BRK_SKIP_USER_BUILD:-} ]]; then
	make -C "$BRK_USER_ROOT" ${CROSS_COMPILE:+CROSS_COMPILE="$CROSS_COMPILE"}
fi

BLK=1024
if ((BRK_ROOTFS_BYTES % BLK != 0)); then
	echo "${0##*/}: BRK_ROOTFS_BYTES ($BRK_ROOTFS_BYTES) must be a multiple of $BLK" >&2
	exit 1
fi
COUNT=$((BRK_ROOTFS_BYTES / BLK))

mkdir -p "$(dirname "$IMG")"
rm -f "$IMG"
dd if=/dev/zero of="$IMG" bs=$BLK count=$COUNT status=none
"$MKFS" "$IMG"

mapfile -t bins < <(ls $BRK_USER_ROOT/build/bin)

echo "Copying user binaries to $IMG"
echo "Binaries: ${bins[@]}"

for name in "${bins[@]}"; do
	src=$BRK_USER_ROOT/build/bin/$name
	"$CP" -r "$src" "/bin/$name" "$IMG"
done

echo "Copying README.md to $IMG"
"$CP" "$BRK_KERNEL_ROOT/README.md" "/README.md" "$IMG"

echo "Copying LICENSE to $IMG"
"$CP" "$BRK_KERNEL_ROOT/LICENSE" "/LICENSE" "$IMG"

echo "${0##*/}: wrote $IMG (${BRK_ROOTFS_BYTES} bytes, brkfs)"

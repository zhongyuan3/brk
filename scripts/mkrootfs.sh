#!/usr/bin/env bash

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

BLOCK_SIZE=4096
BLOCK_COUNT=1024

mkdir -p "$(dirname "$IMG")"
rm -f "$IMG"
dd if=/dev/zero of="$IMG" bs=$BLOCK_SIZE count=$BLOCK_COUNT status=none
"$MKFS" -bs $BLOCK_SIZE "$IMG"

mapfile -t bins < <(ls $BRK_USER_ROOT/build/bin)

for name in "${bins[@]}"; do
	src=$BRK_USER_ROOT/build/bin/$name
	"$CP" -r "$src" "/bin/$name" "$IMG"
done
"$CP" "$BRK_KERNEL_ROOT/README.md" "/README.md" "$IMG"
"$CP" "$BRK_KERNEL_ROOT/LICENSE" "/LICENSE" "$IMG"

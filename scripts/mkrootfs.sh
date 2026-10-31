#!/bin/bash

set -euo pipefail

usage() {
	echo "usage: ${0##*/} <output-rootfs.img>" >&2
	exit 1
}

[[ $# -eq 1 ]] || usage
IMG=$1

KERNEL_ROOT=$(cd "$(dirname "$0")/.." && pwd)
USER_ROOT=${USER_ROOT:-"$KERNEL_ROOT/../brk-user"}
TOOLS_ROOT=${TOOLS_ROOT:-"$KERNEL_ROOT/../brk-tools"}

for d in "$USER_ROOT" "$TOOLS_ROOT"; do
	if [[ ! -d $d ]]; then
		echo "${0##*/}: missing directory: $d" >&2
		echo "Set USER_ROOT / TOOLS_ROOT if your layout differs." >&2
		exit 1
	fi
done

BRKFS=$TOOLS_ROOT/brkfs
if [[ ! -x $BRKFS ]]; then
	if [[ -n ${SKIP_TOOLS_BUILD:-} ]]; then
		echo "${0##*/}: $BRKFS not found (SKIP_TOOLS_BUILD is set)" >&2
		exit 1
	fi
	make -C "$TOOLS_ROOT"
fi
[[ -x $BRKFS ]] || {
	echo "${0##*/}: expected host tool: $BRKFS" >&2
	exit 1
}

if [[ -z ${SKIP_USER_BUILD:-} ]]; then
	make -C "$USER_ROOT" ${CROSS_COMPILE:+CROSS_COMPILE="$CROSS_COMPILE"}
fi

BLOCK_SIZE=4096
BLOCK_COUNT=2048

mkdir -p "$(dirname "$IMG")"
rm -f "$IMG"
dd if=/dev/zero of="$IMG" bs=$BLOCK_SIZE count=$BLOCK_COUNT status=none
"$BRKFS" mkfs -bs $BLOCK_SIZE "$IMG"

mapfile -t bins < <(ls $USER_ROOT/build/64/bin)

for name in "${bins[@]}"; do
	src=$USER_ROOT/build/64/bin/$name
	"$BRKFS" cp --parents "$src" "/bin/$name" "$IMG"
done
"$BRKFS" cp "$KERNEL_ROOT/README.md" "/README.md" "$IMG"
"$BRKFS" cp "$KERNEL_ROOT/LICENSE" "/LICENSE" "$IMG"

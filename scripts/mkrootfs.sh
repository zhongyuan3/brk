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

# Build a small FHS-style directory skeleton.  Git cannot track empty
# directories, so create them here and copy them into the image.
TREE=$KERNEL_ROOT/build/riscv/rootfs-tree
rm -rf "$TREE"
mkdir -p "$TREE"/{bin,sbin,etc,home,root,tmp,boot,dev,proc,sys}
mkdir -p "$TREE"/run/lock
mkdir -p "$TREE"/{mnt,media,opt,srv,lib,lib64}
mkdir -p "$TREE"/usr/{bin,sbin,lib,include,share}
mkdir -p "$TREE"/usr/local/{bin,sbin,lib,include,share}
mkdir -p "$TREE"/usr/share/doc/brk
mkdir -p "$TREE"/var/{log,tmp,spool,cache,lib}

for d in bin sbin etc home root tmp boot dev proc sys run mnt media opt srv \
	 lib lib64 usr var; do
	"$BRKFS" cp -r --parents "$TREE/$d" "/$d" "$IMG"
done

BIN_DIR=$USER_ROOT/build/64/bin
SYSTEM_BINS=(init)
USR_BINS=(brktest cap clear df edit fallocate nc netstat stat tput)

for name in "${SYSTEM_BINS[@]}"; do
	"$BRKFS" cp --parents "$BIN_DIR/$name" "/sbin/$name" "$IMG"
done

for name in "${USR_BINS[@]}"; do
	"$BRKFS" cp --parents "$BIN_DIR/$name" "/usr/bin/$name" "$IMG"
done

mapfile -t bins < <(
	cd "$USER_ROOT/src" || exit 1
	for f in *.c; do
		printf '%s\n' "${f%.c}"
	done
	printf 'sh\n'
)
for name in "${bins[@]}"; do
	case " ${SYSTEM_BINS[*]} ${USR_BINS[*]} " in
	*" $name "*)
		continue
		;;
	esac
	"$BRKFS" cp --parents "$BIN_DIR/$name" "/bin/$name" "$IMG"
done

if [[ -d $USER_ROOT/etc ]]; then
	"$BRKFS" cp -r --parents "$USER_ROOT/etc" "/etc" "$IMG"
fi

for doc in README.md README.cn.md LICENSE; do
	if [[ -f $KERNEL_ROOT/$doc ]]; then
		"$BRKFS" cp --parents "$KERNEL_ROOT/$doc" \
			"/usr/share/doc/brk/$doc" "$IMG"
	fi
done

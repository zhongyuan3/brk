#!/bin/sh
# Detect RISC-V cross toolchain paths and write .brk-toolchain.mk cache.
set -eu

cache=$1
mode=${2:-cross}
cross=${CROSS_COMPILE:-}

prefixes="
riscv64-unknown-elf-
riscv64-elf-
riscv64-none-elf-
riscv64-linux-gnu-
riscv64-unknown-linux-gnu-
"

detect_cross() {
	mkdir -p "$(dirname "$cache")"
	if [ -n "$cross" ]; then
		gcc=$(command -v "${cross}gcc" 2>/dev/null) || return 1
	else
		for p in $prefixes; do
			if command -v "${p}gcc" >/dev/null 2>&1 &&
			   "${p}gcc" -dumpmachine 2>/dev/null | grep -q '^riscv64'; then
				cross=$p
				gcc=$(command -v "${p}gcc")
				break
			fi
		done
		[ -n "${cross:-}" ] || return 1
	fi

	"$gcc" -dumpmachine 2>/dev/null | grep -q '^riscv64' || return 1

	{
		printf 'CROSS_COMPILE := %s\n' "$cross"
		printf 'BRK_CROSS_GCC := %s\n' "$gcc"
	} > "${cache}.tmp"

	if [ -f "$cache" ] && grep -q '^GDB :=' "$cache" 2>/dev/null; then
		grep '^GDB :=' "$cache" >> "${cache}.tmp"
	fi

	mv "${cache}.tmp" "$cache"
	echo ok
}

detect_gdb() {
	mkdir -p "$(dirname "$cache")"
	[ -n "$cross" ] || return 1

	gdb=
	for g in "${cross}gdb" gdb-multiarch; do
		if command -v "$g" >/dev/null 2>&1; then
			if [ "$g" = "${cross}gdb" ]; then
				gdb=$(command -v "$g")
				break
			fi
			if "$g" --batch -quiet -ex "set architecture riscv:rv64" -ex quit >/dev/null 2>&1; then
				gdb=$(command -v "$g")
				break
			fi
		fi
	done
	[ -n "$gdb" ] || return 1

	if [ -f "$cache" ]; then
		grep -v '^GDB :=' "$cache" > "${cache}.tmp" || true
		mv "${cache}.tmp" "$cache"
	fi
	printf 'GDB := %s\n' "$gdb" >> "$cache"
	echo ok
}

case "$mode" in
cross) detect_cross ;;
gdb) detect_gdb ;;
*) return 1 2>/dev/null || exit 1 ;;
esac

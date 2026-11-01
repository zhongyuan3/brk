#!/bin/sh
#
# Verify header layering rules:
#   - every uapi/*.h compiles standalone with no libc and no brk/ headers;
#   - no uapi header includes a kernel-internal brk/ header;
#   - every brk/*.h compiles standalone from the kernel include path;
#   - the UAPI ABI assertions in src/kernel/abi_check.c still hold.
#
# Run from the repository root (the Makefile target does this).

set -eu

CC=${CC:-${CROSS_COMPILE:-}gcc}
ARCH=${ARCH:-riscv}
# Do not inherit the kernel's CFLAGS: those contain -MMD/-MP and would drop
# dependency files next to the temporary translation units.
CFLAGS=${BRK_HDR_CFLAGS:--std=gnu11 -Wall -Wextra -Werror -ffreestanding -nostdinc -fno-common}
INC="-Isrc/arch/$ARCH/include -Isrc/include -Isrc/include/freestanding -Ivendor/libfdt"

tmpdir=$(mktemp -d "${TMPDIR:-/tmp}/brk-headers.XXXXXX")
trap 'rm -rf "$tmpdir"' EXIT INT TERM

failed=0

report() {
	printf '%s\n' "$*" >&2
	failed=1
}

check_uapi() {
	# Isolate the UAPI directory: only <uapi/...> may resolve.  This catches
	# accidental dependencies on <stdint.h>, <string.h>, <brk/...>, etc.
	uapi_root=$tmpdir/uapi-root
	mkdir -p "$uapi_root"
	ln -s "$(pwd)/src/include/uapi" "$uapi_root/uapi"

	for h in src/include/uapi/*.h; do
		name=${h#src/include/}
		tu=$tmpdir/uapi.c
		printf '#include <%s>\n' "$name" >"$tu"
		if ! $CC $CFLAGS -I"$uapi_root" -fsyntax-only "$tu" 2>"$tmpdir/err"; then
			report "FAIL: $name is not self-contained"
			sed 's/^/  /' "$tmpdir/err" >&2
		fi
	done
}

check_no_brk_in_uapi() {
	if grep -rn -E '#[[:space:]]*include[[:space:]]*<brk/' src/include/uapi; then
		report "FAIL: UAPI headers must not include <brk/...>"
	fi
}

check_freestanding() {
	# The freestanding layer must depend only on the compiler, never on
	# kernel-internal or UAPI headers.
	if grep -rn -E '#[[:space:]]*include[[:space:]]*<(brk|uapi)/' \
		src/include/freestanding; then
		report "FAIL: freestanding headers must not include brk/ or uapi/"
	fi

	for h in src/include/freestanding/*.h; do
		name=${h#src/include/freestanding/}
		tu=$tmpdir/freestanding.c
		printf '#include <%s>\n' "$name" >"$tu"
		if ! $CC $CFLAGS -Isrc/include/freestanding \
			-fsyntax-only "$tu" 2>"$tmpdir/err"; then
			report "FAIL: freestanding/$name does not compile standalone"
			sed 's/^/  /' "$tmpdir/err" >&2
		fi
	done
}

check_brk() {
	for h in src/include/brk/*.h src/include/brk/*/*.h \
		 src/arch/$ARCH/include/arch/*.h \
		 src/arch/$ARCH/include/arch/*/*.h; do
		[ -f "$h" ] || continue
		case $h in
		src/arch/*) name=${h#src/arch/$ARCH/include/} ;;
		*) name=${h#src/include/} ;;
		esac
		tu=$tmpdir/brk.c
		printf '#include <%s>\n' "$name" >"$tu"
		if ! $CC $CFLAGS $INC -fsyntax-only "$tu" 2>"$tmpdir/err"; then
			report "FAIL: $name does not compile standalone"
			sed 's/^/  /' "$tmpdir/err" >&2
		fi
	done
}

check_abi() {
	if ! $CC $CFLAGS $INC -fsyntax-only src/kernel/abi_check.c 2>"$tmpdir/err"; then
		report "FAIL: UAPI ABI assertions in src/kernel/abi_check.c"
		sed 's/^/  /' "$tmpdir/err" >&2
	fi
}

check_uapi
check_no_brk_in_uapi
check_freestanding
check_brk
check_abi

if [ "$failed" -ne 0 ]; then
	exit 1
fi

echo "headers: ok"

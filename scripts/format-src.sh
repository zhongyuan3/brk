#!/bin/bash
#
# format-src.sh - Recursively format C sources and headers under src/.
#
# Applies the project's .clang-format (Linux kernel style) to every *.c and
# *.h file found under src/, skipping vendor/ directories anywhere in the
# tree. Requires clang-format >= 11.
#
# Usage: scripts/format-src.sh [--dry-run] [path]
#   --dry-run   Only list the files that would be formatted.
#   path        Directory to format (default: <repo>/src).
#

set -eu

root=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
dry_run=0
srcdir=

for arg in "$@"; do
	case "$arg" in
	--dry-run | -n) dry_run=1 ;;
	-*)
		echo "error: unknown option: $arg" >&2
		exit 1
		;;
	*)
		srcdir=$arg
		;;
	esac
done

srcdir=${srcdir:-$root/src}
srcdir=$(cd "$srcdir" && pwd)

if ! command -v clang-format >/dev/null 2>&1; then
	echo "error: clang-format not found in PATH" >&2
	exit 1
fi

if [ ! -d "$srcdir" ]; then
	echo "error: directory not found: $srcdir" >&2
	exit 1
fi

# Collect C sources/headers, pruning any vendor/ directory in the tree.
files=()
while IFS= read -r -d '' f; do
	files+=("$f")
done < <(find "$srcdir" -type d -name vendor -prune -o \
	-type f \( -name '*.c' -o -name '*.h' \) -print0)

count=${#files[@]}
if [ "$count" -eq 0 ]; then
	echo "No source files found in $srcdir."
	exit 0
fi

echo "Found $count file(s) under $srcdir (vendor/ excluded)."

if [ "$dry_run" -eq 1 ]; then
	printf '%s\n' "${files[@]}"
	exit 0
fi

# Format in place, in parallel.
printf '%s\0' "${files[@]}" | xargs -0 -r -P "$(nproc)" clang-format \
	-i --style=file

echo "Formatting complete."

#!/bin/bash
#
# smoke.sh - boot a BRK kernel in QEMU and verify that the shell runs a command.
#
# Required environment:
#   QEMU       - QEMU binary
#   QEMU_OPTS  - QEMU arguments, including the rootfs drive
#
# Usage: scripts/smoke.sh <kernel> <log>
#
# Optional environment:
#   SMOKE_BOOT_DELAY  seconds to wait for the shell (default: 6)
#   SMOKE_TIMEOUT     overall QEMU timeout in seconds (default: 30)
#

set -u

kernel=${1:?usage: smoke.sh <kernel> <log>}
log=${2:?usage: smoke.sh <kernel> <log>}
: "${QEMU:?QEMU is not set}"
: "${QEMU_OPTS:?QEMU_OPTS is not set}"

boot_delay=${SMOKE_BOOT_DELAY:-6}
timeout_secs=${SMOKE_TIMEOUT:-30}
marker=smoke-ok

mkdir -p "$(dirname "$log")"
rm -f "$log"

# QEMU_OPTS is intentionally word-split: the Makefile builds it from plain
# argument strings and this script is the only consumer.
{
	sleep "$boot_delay"
	printf 'echo %s\n' "$marker"
	sleep 2
} | timeout "$timeout_secs" $QEMU $QEMU_OPTS -kernel "$kernel" \
	>"$log" 2>&1 || true

if grep -a -q "^${marker}" "$log"; then
	echo "smoke: ok"
	exit 0
fi

echo "smoke: failed; log: $log" >&2
tail -40 "$log" | cat -v >&2 || true
exit 1

#!/usr/bin/env bash
set -euo pipefail

repo="$(cd "$(dirname "$0")/.." && pwd)"
source_disk="$repo/build/sparcv9/hdd-image.img"
disk="$(mktemp)"
log="$(mktemp)"
clean_log="$(mktemp)"
trap 'rm -f "$log" "$clean_log" "$disk"' EXIT

command -v qemu-system-sparc64 >/dev/null || {
	echo "qemu-system-sparc64 missing" >&2
	exit 1
}
test -f "$source_disk" || {
	echo "missing SPARC V9 image: $source_disk" >&2
	exit 1
}
cp --reflink=auto "$source_disk" "$disk"

set +e
{
	sleep 8
	printf 'echo SPARCV9-USER64-PASS\n'
	printf 'pwd\n'
	printf 'ls /\n'
	printf 'cp /sparcv9/bin/sh /SPARCTST\n'
	printf 'ls /\n'
	sleep 2
} | timeout 20s qemu-system-sparc64 -M sun4u -m 256M \
	-drive "file=$disk,format=raw,if=ide" \
	-nographic -no-reboot >"$log" 2>&1
status=$?
set -e
if test "$status" -ne 0 && test "$status" -ne 124; then
	cat "$log" >&2
	exit "$status"
fi
cat "$log"
tr -d '\r' <"$log" >"$clean_log"
grep -q 'SPARCV9 IDE PASS' "$clean_log"
grep -q 'boot: starting init /sparcv9/bin/sh' "$clean_log"
grep -qx 'SPARCV9-USER64-PASS' "$clean_log"
grep -q 'vmunix.s9' "$clean_log"
grep -q 'sparctst' "$clean_log"
if grep -q '^cp: ' "$clean_log" || grep -q '^fatal: ' "$clean_log" || \
    grep -qx 'error' "$clean_log"; then
	echo "SPARC V9 filesystem command failed" >&2
	exit 1
fi
echo "SPARC V9 sun4u disk/FAT16/user64 test: PASS"

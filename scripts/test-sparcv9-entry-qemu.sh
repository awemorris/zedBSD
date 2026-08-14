#!/usr/bin/env bash
set -euo pipefail

repo="$(cd "$(dirname "$0")/.." && pwd)"
image="$repo/build/sparcv9/hdd-image.img"
log="$(mktemp)"
trap 'rm -f "$log"' EXIT

command -v qemu-system-sparc64 >/dev/null || {
	echo "qemu-system-sparc64 missing" >&2
	exit 1
}
test -f "$image" || { echo "missing SPARC V9 image: $image" >&2; exit 1; }

set +e
timeout 12s qemu-system-sparc64 -M sun4u -m 256M \
	-drive "file=$image,format=raw,if=ide" \
	-nographic -no-reboot >"$log" 2>&1
status=$?
set -e
if test "$status" -ne 0 && test "$status" -ne 124; then
	cat "$log" >&2
	exit "$status"
fi
cat "$log"
for marker in \
	'SPARCV9 STAGE1' 'SPARCV9 STAGE2' 'SPARCV9 ELF PASS' \
	'SPARCV9 ENTRY' 'SPARCV9 WINDOW PASS' 'SPARCV9 TRAP PASS' \
	'SPARCV9 MMU PASS' 'SPARCV9 PAGING PASS' \
	'SPARCV9 CONTEXT PASS' 'SPARCV9 TIMER PASS'; do
	grep -q "$marker" "$log" || {
		echo "missing SPARC V9 entry marker: $marker" >&2
		exit 1
	}
done
echo "SPARC V9 sun4u entry test: PASS"

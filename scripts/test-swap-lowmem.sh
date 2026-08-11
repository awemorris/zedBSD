#!/usr/bin/env bash
set -euo pipefail

repo="$(cd "$(dirname "$0")/.." && pwd)"
qemu="${QEMU:-/home/awe/qemu-pc98/build/qemu-system-i386}"
bios_dir="${PC98_BIOS_DIR:-/home/awe/qemu-pc98/roms/pc98bios}"
work="${ZEDBSD_BUILD_DIR:-$repo/build/pc98}/tests/swap-lowmem"

test -x "$qemu" || { echo "QEMU not found: $qemu" >&2; exit 1; }
test -d "$bios_dir" || { echo "BIOS directory not found: $bios_dir" >&2; exit 1; }
mkdir -p "$work"

run_required()
{
	local memory="$1"
	echo "==> zedBSD swap pressure: ${memory} MiB"
	QEMU="$qemu" PC98_BIOS_DIR="$bios_dir" \
		ZEDBSD_USER_TEST_MODE=swap ZEDBSD_USER_REQUIRE_GUI=0 \
		ZEDBSD_QEMU_MEMORY="$memory" \
		"$repo/scripts/test-user-init.sh" | tee "$work/${memory}mib.log"
}

run_required 8
run_required 5

# zedBSD currently requires at least 4 MiB before pageable user memory is
# considered.  Exercise the closest QEMU whole-MiB setting without making
# that documented nonpageable-floor measurement a required success.
stretch="${ZEDBSD_SWAP_STRETCH_MEMORY:-3}"
echo "==> zedBSD swap stretch probe: ${stretch} MiB"
if QEMU="$qemu" PC98_BIOS_DIR="$bios_dir" \
	ZEDBSD_USER_TEST_MODE=swap ZEDBSD_USER_REQUIRE_GUI=0 \
	ZEDBSD_QEMU_MEMORY="$stretch" \
	"$repo/scripts/test-user-init.sh" >"$work/${stretch}mib.log" 2>&1; then
	echo "stretch probe unexpectedly completed at ${stretch} MiB"
else
	echo "stretch probe reached the documented nonpageable floor at ${stretch} MiB"
fi

echo "zedBSD low-memory swap test: PASS"

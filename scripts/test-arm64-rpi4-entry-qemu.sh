#!/usr/bin/env bash
set -euo pipefail

repo="$(cd "$(dirname "$0")/.." && pwd)"
image="$repo/build/arm64/VMUNIX.A64"
dtb="$repo/vendor/raspberrypi-firmware/boot/bcm2711-rpi-4-b.dtb"
log="$(mktemp)"
trap 'rm -f "$log"' EXIT

command -v qemu-system-aarch64 >/dev/null || { echo "qemu-system-aarch64 missing" >&2; exit 1; }
test -f "$dtb" || {
	echo "missing Pi 4 DTB; run: git submodule update --init vendor/raspberrypi-firmware" >&2
	exit 1
}
qemu-system-aarch64 -machine help | grep -q '^raspi4b ' || {
	echo "QEMU does not provide the raspi4b machine" >&2; exit 1;
}

set +e
# QEMU's raspi4b model exposes four cores unconditionally; locore parks 1..3.
timeout 10s qemu-system-aarch64 -M raspi4b -smp 4 -m 2G \
	-kernel "$image" -serial stdio -display none -monitor none \
	-dtb "$dtb" \
	-no-reboot >"$log" 2>&1
status=$?
set -e
if test "$status" -ne 0 && test "$status" -ne 124; then
	cat "$log" >&2
	exit "$status"
fi
cat "$log"
grep -q 'RPI4 ENTRY' "$log"
grep -q 'RPI4 EL1 PASS' "$log"
grep -q 'RPI4 FDT PASS' "$log"
grep -q 'ARM64 PAGING PASS' "$log"
grep -q 'ARM64 CONTEXT PASS' "$log"
grep -q 'ARM64 IRQ READY' "$log"
grep -q 'ARM64 TIMER TICK' "$log"
echo "arm64 Raspberry Pi 4 entry test: PASS"

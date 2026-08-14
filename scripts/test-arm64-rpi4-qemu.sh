#!/usr/bin/env bash
set -euo pipefail

repo="$(cd "$(dirname "$0")/.." && pwd)"
kernel="$repo/build/arm64/VMUNIX.A64"
source_disk="$repo/build/arm64/hdd-image.img"
disk="$(mktemp)"
dtb="$repo/vendor/raspberrypi-firmware/boot/bcm2711-rpi-4-b.dtb"
log="$(mktemp)"
trap 'rm -f "$log" "$disk"' EXIT

for file in "$kernel" "$source_disk" "$dtb"; do
	test -f "$file" || { echo "missing Pi 4 test input: $file" >&2; exit 1; }
done
cp --reflink=auto "$source_disk" "$disk"
command -v qemu-system-aarch64 >/dev/null || {
	echo "qemu-system-aarch64 missing" >&2; exit 1;
}

set +e
{
	sleep 3
	printf 'echo RPI4-USERLAND-PASS\n'
	printf 'cp /config.txt /rpi4.txt\n'
	printf 'cat /rpi4.txt\n'
	sleep 2
} | timeout 15s qemu-system-aarch64 -M raspi4b -smp 4 -m 2G \
	-kernel "$kernel" -drive "file=$disk,if=sd,format=raw" \
	-serial stdio -display none -monitor none -dtb "$dtb" -no-reboot \
	>"$log" 2>&1
status=$?
set -e
if test "$status" -ne 0 && test "$status" -ne 124; then
	cat "$log" >&2
	exit "$status"
fi
cat "$log"
grep -q 'RPI4 FRAMEBUFFER PASS' "$log"
grep -q 'ARM64 TIMER TICK' "$log"
grep -q 'sdhci: sd0 ready' "$log"
grep -q 'vfs: sd0 partition 1' "$log"
grep -q 'RPI4-USERLAND-PASS' "$log"
grep -q 'hdmi_force_hotplug=1' "$log"
echo "arm64 Raspberry Pi 4 SD/FAT16/EL0 test: PASS"

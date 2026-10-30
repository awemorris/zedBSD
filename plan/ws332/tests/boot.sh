#!/bin/sh
# The boot check of the tick conversions: builds each port from this folder's config and boots it with
# plan/tools/boot-test.sh (the screen, never the serial log), one summary line a port and the screenshots.
#   plan/ws332/tests/boot.sh [OUT] [PORT...]     PORT: amd64 pcat rpi4 riscv64-virt m68k-virt (default all)
# OUT (default build/ws332-boot) holds BUILD folders (OUT/build-PORT) and the boot-test outputs (OUT/PORT).
# The image builds take /tmp/zedbsd-image-build.lock.  The last line is "ws332-boot: PASS" or "ws332-boot: FAIL".
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
set -u
cd "$(dirname -- "$0")/../../.."
out=${1:-build/ws332-boot}
[ $# -gt 0 ] && shift
ports=${*:-"amd64 pcat rpi4 riscv64-virt m68k-virt"}
mkdir -p "$out"
summary=$out/summary.txt
: > "$summary"
failed=0

# Builds one port's artefact under the image lock; the log is OUT/PORT-build.log.
build() {
	port=$1
	shift
	if timeout 5400 flock /tmp/zedbsd-image-build.lock \
	    make -j16 ZEDBSD_CONFIG="plan/ws332/tests/config-$port.mk" BUILD="$out/build-$port" "$@" \
	    > "$out/$port-build.log" 2>&1; then
		return 0
	fi
	echo "$port: build FAIL ($out/$port-build.log)" >> "$summary"
	return 1
}

# Boots one port and notes the result.
boot() {
	port=$1
	shift
	mkdir -p "$out/$port/work"
	if env OUTPUT="$out/$port" BOOT_TEST_WORK="$out/$port/work" "$@" > "$out/$port-boot.log" 2>&1; then
		echo "$port: boot PASS ($out/$port)" >> "$summary"
		return 0
	fi
	echo "$port: boot FAIL ($out/$port-boot.log)" >> "$summary"
	return 1
}

for port in $ports; do
	case $port in
	amd64)
		build amd64 disk-image && boot amd64 BOOT_TIMEOUT=300 \
		    plan/tools/boot-test.sh "$out/build-amd64/hdd-image.img" || failed=1
		;;
	pcat)
		build pcat disk-image && boot pcat BOOT_MODE=bios-ide BOOT_TIMEOUT=600 \
		    plan/tools/boot-test.sh "$out/build-pcat/hdd-image.img" || failed=1
		;;
	rpi4)
		build rpi4 disk-image && boot rpi4 BOOT_MODE=raspi4b BOOT_TIMEOUT=900 \
		    plan/tools/boot-test.sh "$out/build-rpi4/hdd-image.img" || failed=1
		;;
	riscv64-virt)
		build riscv64-virt "$out/build-riscv64-virt/vmunix" && boot riscv64-virt BOOT_MODE=riscv64-virt \
		    BOOT_TIMEOUT=300 BOOT_PATTERN="$(printf 'TIMER TICK\nVFS initialization failed')" \
		    plan/tools/boot-test.sh "$out/build-riscv64-virt/vmunix" || failed=1
		;;
	m68k-virt)
		build m68k-virt "$out/build-m68k-virt/vmunix" && boot m68k-virt BOOT_MODE=m68k-virt \
		    BOOT_TIMEOUT=300 BOOT_PATTERN="$(printf 'M68040 HAL READY.*TIMER TICK\nVFS initialization failed')" \
		    KERNEL="$out/build-m68k-virt/vmunix" plan/tools/boot-test.sh "$out/build-m68k-virt/vmunix" || failed=1
		;;
	*)
		echo "$port: unknown port" >> "$summary"
		failed=1
		;;
	esac
done

cat "$summary"
if [ $failed -eq 0 ]; then
	echo "ws332-boot: PASS"
else
	echo "ws332-boot: FAIL"
fi

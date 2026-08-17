#!/usr/bin/env bash
# POSIX R2 user/kernel integration test. Copyright (C) 2026 Awe Morris.
# SPDX-License-Identifier: Zlib
set -euo pipefail

repo="$(cd "$(dirname "$0")/.." && pwd)"
arch="${1:-all}"
test_elf="${ZEDBSD_POSIX_TEST_ELF:-POSIX-R2.ELF}"
test_marker="${ZEDBSD_POSIX_TEST_MARKER:-R2:01-06:PASS}"
test_failure="${ZEDBSD_POSIX_TEST_FAILURE:-POSIX_R2_FAIL:}"
test_label="${ZEDBSD_POSIX_TEST_LABEL:-POSIX R2}"
work="$(mktemp -d "${TMPDIR:-/tmp}/zedbsd-posix-r2.XXXXXX")"
qemu_pid=

cleanup()
{
	if test -n "$qemu_pid"; then
		kill "$qemu_pid" 2>/dev/null || true
		wait "$qemu_pid" 2>/dev/null || true
	fi
	rm -rf "$work"
}
trap cleanup EXIT INT TERM

install_test_elf()
{
	local platform="$1" image="$2" profile inner spec
	case "$platform" in
	pc98) profile=i386 ;;
	pcat) profile=i386 ;;
	amd64) profile=amd64 ;;
	arm64) profile=aarch64 ;;
	*) echo "unsupported overlay platform: $platform" >&2; exit 2 ;;
	esac
	spec="$image@@$((2048 * 512))"
	inner="$work/$platform-profile.img"
	mcopy -o -i "$spec" "$repo/build/$platform/$test_elf" ::/init.elf
	mcopy -i "$spec" ::/arch/"$profile.img" "$inner"
	mcopy -o -i "$inner" "$repo/build/$platform/$test_elf" ::/bin/sh
	mcopy -o -i "$spec" "$inner" ::/arch/"$profile.img"
}

run_pc98()
{
	local image="$work/pc98.img" log="$work/pc98.log"
	local qmp="$work/pc98.qmp" dump="$work/pc98-tvram.bin"
	local qemu="${QEMU_PC98:-$HOME/qemu-pc98/build/qemu-system-i386}"
	local bios="${PC98_BIOS_DIR:-$HOME/qemu-pc98/roms/pc98bios}"
	test -x "$qemu" || { echo "PC-98 QEMU not found: $qemu" >&2; exit 1; }
	test -d "$bios" || { echo "PC-98 BIOS directory not found: $bios" >&2; exit 1; }
	"$repo/build.sh" hdd-image pc98
	"$repo/build.sh" "$test_elf" pc98
	cp --reflink=auto "$repo/build/pc98/hdd-image.img" "$image"
	install_test_elf pc98 "$image"
	"$qemu" -M pc9821 -cpu 486 -m 64 -accel tcg -L "$bios" \
		-nic none -display none -serial none -monitor none -snapshot \
		-no-reboot -no-shutdown -qmp "unix:$qmp,server=on,wait=off" \
		-drive "if=ide,bus=0,unit=0,format=raw,file=$image" \
		>"$log" 2>&1 &
	qemu_pid=$!
	if ! python3 "$repo/scripts/wait-pc98-screen-marker.py" --qmp "$qmp" \
	    --dump "$dump" --marker "$test_marker" \
	    --timeout-ms "${ZEDBSD_POSIX_TIMEOUT_MS:-60000}"; then
		cat "$log" >&2
		echo "$test_label QEMU failed: pc98" >&2
		exit 1
	fi
	kill "$qemu_pid" 2>/dev/null || true
	wait "$qemu_pid" 2>/dev/null || true
	qemu_pid=
	echo "$test_label QEMU: PASS (pc98)"
}

wait_for_marker()
{
	local log="$1" label="$2" found=0
	for _ in $(seq 1 400); do
		if test -f "$log" && grep -Fq "$test_marker" "$log"; then
			found=1
			break
		fi
		if test -f "$log" && grep -Fq "$test_failure" "$log"; then
			break
		fi
		if ! kill -0 "$qemu_pid" 2>/dev/null; then
			break
		fi
		sleep 0.1
	done
	kill "$qemu_pid" 2>/dev/null || true
	wait "$qemu_pid" 2>/dev/null || true
	qemu_pid=
	if test "$found" -ne 1 || grep -Fq 'fatal:' "$log"; then
		test -f "$log" && cat "$log" >&2
		echo "$test_label QEMU failed: $label" >&2
		exit 1
	fi
	echo "$test_label QEMU: PASS ($label)"
}

run_x86()
{
	local platform="$1" qemu="$2" cpu="$3"
	local image="$work/$platform.img" log="$work/$platform.log"
	"$repo/build.sh" hdd-image "$platform"
	"$repo/build.sh" "$test_elf" "$platform"
	cp --reflink=auto "$repo/build/$platform/hdd-image.img" "$image"
	install_test_elf "$platform" "$image"
	"$qemu" -M pc -cpu "$cpu" -m 64M -accel tcg -nic none \
		-display none -serial none -monitor none -snapshot \
		-no-reboot -no-shutdown -debugcon "file:$log" \
		-global isa-debugcon.iobase=0xe9 \
		-drive "if=ide,format=raw,file=$image" >/dev/null 2>&1 &
	qemu_pid=$!
	wait_for_marker "$log" "$platform"
}

run_arm64()
{
	local image="$work/arm64.img" log="$work/arm64.log"
	local dtb="$repo/vendor/raspberrypi-firmware/boot/bcm2711-rpi-4-b.dtb"
	"$repo/build.sh" hdd-image arm64
	"$repo/build.sh" "$test_elf" arm64
	cp --reflink=auto "$repo/build/arm64/hdd-image.img" "$image"
	install_test_elf arm64 "$image"
	qemu-system-aarch64 -M raspi4b -smp 4 -m 2G \
		-kernel "$repo/build/arm64/VMUNIX.A64" \
		-drive "file=$image,if=sd,format=raw" -serial "file:$log" \
		-display none -monitor none -dtb "$dtb" -no-reboot \
		-no-shutdown >/dev/null 2>&1 &
	qemu_pid=$!
	wait_for_marker "$log" arm64
}

run_sparcv9()
{
	local image="$work/sparcv9.img" log="$work/sparcv9.log"
	"$repo/build.sh" all sparcv9
	"$repo/build.sh" "$test_elf" sparcv9
	python3 "$repo/scripts/make-sparcv9-hdd-image.py" --force \
		--stage1 "$repo/build/sparcv9/boot/stage1.bin" \
		--stage2 "$repo/build/sparcv9/boot/stage2.bin" \
		--kernel "$repo/build/sparcv9/vmunix" \
		--shell "$repo/build/sparcv9/$test_elf" "$image"
	mcopy -o -i "$image@@$((4096 * 512))" \
		"$repo/build/sparcv9/$test_elf" ::/init.elf
	qemu-system-sparc64 -M sun4u -m 256M \
		-drive "file=$image,format=raw,if=ide" -nographic -no-reboot \
		-no-shutdown >"$log" 2>&1 &
	qemu_pid=$!
	wait_for_marker "$log" sparcv9
}

case "$arch" in
pc98) run_pc98 ;;
pcat) run_x86 pcat "${QEMU_SYSTEM_I386:-qemu-system-i386}" 486 ;;
amd64) run_x86 amd64 "${QEMU_PCAT_X86_64:-qemu-system-x86_64}" qemu64 ;;
arm64) run_arm64 ;;
sparcv9) run_sparcv9 ;;
all)
	run_pc98
	run_x86 pcat "${QEMU_SYSTEM_I386:-qemu-system-i386}" 486
	run_x86 amd64 "${QEMU_PCAT_X86_64:-qemu-system-x86_64}" qemu64
	run_arm64
	run_sparcv9
	;;
*) echo "usage: $0 [all|pc98|pcat|amd64|arm64|sparcv9]" >&2; exit 2 ;;
esac

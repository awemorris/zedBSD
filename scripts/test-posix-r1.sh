#!/usr/bin/env bash
# POSIX R1 user/kernel integration test for every supported user ABI.
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
set -euo pipefail

repo="$(cd "$(dirname "$0")/.." && pwd)"
arch="${1:-all}"
work="$(mktemp -d "${TMPDIR:-/tmp}/zedbsd-posix-r1.XXXXXX")"
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
	local platform="$1" image="$2" destination="$3"
	local spec="$image@@$((2048 * 512))"
	local profile inner
	case "$platform" in
	pcat) profile=i386 ;;
	amd64) profile=amd64 ;;
	arm64) profile=aarch64 ;;
	*) echo "no architecture profile for POSIX test: $platform" >&2; exit 2 ;;
	esac
	inner="$work/$platform-profile.img"
	mcopy -o -i "$spec" "$repo/build/$platform/POSIX-R1.ELF" ::/init.elf
	mcopy -i "$spec" ::/rootfs.img "$inner"
	mcopy -o -i "$inner" "$repo/build/$platform/POSIX-R1.ELF" ::/bin/sh
	mcopy -o -i "$spec" "$inner" ::/rootfs.img
	# Keep the argument for callers documenting the historical injection
	# point; direct /bin overlays now come from the nested profile image.
	: "$destination"
}

wait_for_marker()
{
	local log="$1" label="$2" found=0
	for _ in $(seq 1 350); do
		if test -f "$log" && grep -Fq 'BOOT_USER_SYSCALL_OK' "$log"; then
			found=1
			break
		fi
		if test -f "$log" && grep -Fq 'POSIX_R1_FAIL:' "$log"; then
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
	if test "$found" -ne 1; then
		test -f "$log" && cat "$log" >&2
		echo "POSIX R1 QEMU timeout: $label" >&2
		exit 1
	fi
	if grep -Fq 'fatal:' "$log"; then
		cat "$log" >&2
		echo "POSIX R1 kernel fatal error: $label" >&2
		exit 1
	fi
	echo "POSIX R1 QEMU: PASS ($label)"
}

run_x86()
{
	local platform="$1" qemu="$2" cpu="$3"
	local image="$work/$platform.img" log="$work/$platform.log"
	"$repo/build.sh" hdd-image "$platform"
	"$repo/build.sh" POSIX-R1.ELF "$platform"
	cp --reflink=auto "$repo/build/$platform/hdd-image.img" "$image"
	install_test_elf "$platform" "$image" ::/bin/sh
	"$qemu" -M pc -cpu "$cpu" -m 64M -accel tcg -nic none \
		-display none -serial none -monitor none -snapshot \
		-no-reboot -no-shutdown \
		-debugcon "file:$log" -global isa-debugcon.iobase=0xe9 \
		-drive "if=ide,format=raw,file=$image" >/dev/null 2>&1 &
	qemu_pid=$!
	wait_for_marker "$log" "$platform"
}

run_arm64()
{
	local platform=arm64 image="$work/arm64.img" log="$work/arm64.log"
	local dtb="$repo/vendor/raspberrypi-firmware/boot/bcm2711-rpi-4-b.dtb"
	"$repo/build.sh" hdd-image "$platform"
	"$repo/build.sh" POSIX-R1.ELF "$platform"
	cp --reflink=auto "$repo/build/arm64/hdd-image.img" "$image"
	install_test_elf "$platform" "$image" ::/arm64/bin/sh
	qemu-system-aarch64 -M raspi4b -smp 4 -m 2G \
		-kernel "$repo/build/arm64/VMUNIX.A64" \
		-drive "file=$image,if=sd,format=raw" -serial "file:$log" \
		-display none -monitor none -dtb "$dtb" -no-reboot \
		-no-shutdown >/dev/null 2>&1 &
	qemu_pid=$!
	wait_for_marker "$log" "$platform"
}

run_sparcv9()
{
	local platform=sparcv9 image="$work/sparcv9.img" log="$work/sparcv9.log"
	local clean_log="$work/sparcv9.clean.log"
	"$repo/build.sh" all "$platform"
	"$repo/build.sh" POSIX-R1.ELF "$platform"
	python3 "$repo/scripts/make-sparcv9-hdd-image.py" --force \
		--stage1 "$repo/build/sparcv9/boot/stage1.bin" \
		--stage2 "$repo/build/sparcv9/boot/stage2.bin" \
		--kernel "$repo/build/sparcv9/vmunix" \
		--shell "$repo/build/sparcv9/POSIX-R1.ELF" "$image"
	mcopy -o -i "$image@@$((4096 * 512))" \
		"$repo/build/sparcv9/POSIX-R1.ELF" ::/init.elf
	qemu-system-sparc64 -M sun4u -m 256M \
		-drive "file=$image,format=raw,if=ide" -nographic -no-reboot \
		-no-shutdown >"$log" 2>&1 &
	qemu_pid=$!
	wait_for_marker "$log" "$platform"
	tr -d '\r' <"$log" >"$clean_log"
	grep -q 'SPARCV9 IDE PASS' "$clean_log"
}

case "$arch" in
pc98)
	ZEDBSD_USER_TEST_MODE=int "$repo/scripts/test-user-init.sh"
	;;
pcat)
	run_x86 pcat "${QEMU_SYSTEM_I386:-qemu-system-i386}" 486
	;;
amd64)
	run_x86 amd64 "${QEMU_PCAT_X86_64:-qemu-system-x86_64}" qemu64
	;;
arm64)
	run_arm64
	;;
sparcv9)
	run_sparcv9
	;;
all)
	ZEDBSD_USER_TEST_MODE=int "$repo/scripts/test-user-init.sh"
	run_x86 pcat "${QEMU_SYSTEM_I386:-qemu-system-i386}" 486
	run_x86 amd64 "${QEMU_PCAT_X86_64:-qemu-system-x86_64}" qemu64
	run_arm64
	run_sparcv9
	;;
*)
	echo "usage: $0 [all|pc98|pcat|amd64|arm64|sparcv9]" >&2
	exit 2
	;;
esac

#!/usr/bin/env bash
# Dynamic-linker QEMU integration test across PC/AT and non-x86 ports.
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
set -euo pipefail

repo="$(cd "$(dirname "$0")/.." && pwd)"
. "$repo/scripts/test-common.sh"

selection="${1:-all}"
work="$(zedbsd_test_make_workdir dynamic-qemu)"
result_dir="${ZEDBSD_TEST_RESULT_DIR:-$repo/build/test-results/dynamic-qemu}"
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

wait_dynamic()
{
	local arch="$1" image="$2" log="$3" start="$4" status_code=0
	local end status
	if zedbsd_test_wait_for_marker "$qemu_pid" "$log" \
	    'DL:06:PLUGIN-TLS' 'fatal:' "${ZEDBSD_DYNAMIC_TIMEOUT_MS:-60000}"; then
		status_code=0
	else
		status_code=$?
	fi
	kill "$qemu_pid" 2>/dev/null || true
	wait "$qemu_pid" 2>/dev/null || true
	qemu_pid=
	end="$(zedbsd_test_utc_now)"
	status="$(zedbsd_test_status_name "$status_code")"
	zedbsd_test_write_result "$result_dir/$arch.json" dynamic-qemu "$arch" \
	    "$status" "$start" "$end" "image_sha256=$(zedbsd_test_sha256 "$image")" \
	    "success_marker=DL:06:PLUGIN-TLS" "log=$log"
	if test "$status_code" -ne 0; then
		test -f "$log" && tr -d '\r' <"$log" >&2
		echo "dynamic QEMU test failed: $arch ($status)" >&2
		return 1
	fi
	if grep -Fq 'fatal:' "$log"; then
		tr -d '\r' <"$log" >&2
		echo "dynamic QEMU test observed a fatal error: $arch" >&2
		return 1
	fi
	echo "dynamic QEMU: PASS ($arch)"
}

wait_dynamic_screen()
{
	local arch="$1" image="$2" log="$3" start="$4" qmp="$5"
	local end status status_code=0
	python3 "$repo/scripts/wait-pc98-screen-marker.py" --qmp "$qmp" \
	    --dump "$work/$arch-tvram.bin" --marker 'DL:06:PLUGIN-TLS' \
	    --timeout-ms "${ZEDBSD_DYNAMIC_TIMEOUT_MS:-60000}" || status_code=$?
	kill "$qemu_pid" 2>/dev/null || true
	wait "$qemu_pid" 2>/dev/null || true
	qemu_pid=
	end="$(zedbsd_test_utc_now)"
	status="$(zedbsd_test_status_name "$status_code")"
	zedbsd_test_write_result "$result_dir/$arch.json" dynamic-qemu "$arch" \
	    "$status" "$start" "$end" "image_sha256=$(zedbsd_test_sha256 "$image")" \
	    "success_marker=DL:06:PLUGIN-TLS" "evidence=PC-98-TVRAM" "log=$log"
	if test "$status_code" -ne 0; then
		test -f "$log" && tr -d '\r' <"$log" >&2
		echo "dynamic QEMU test failed: $arch ($status)" >&2
		return 1
	fi
	echo "dynamic QEMU: PASS ($arch)"
}

prepare_pc_overlay()
{
	local source="$1" image="$2" profile="$3" build_arch="$4" inner="$5"
	local spec
	cp --reflink=auto "$source" "$image"
	spec="$image@@$((2048 * 512))"
	mcopy -i "$spec" "::/arch/$profile.img" "$inner"
	mcopy -o -i "$inner" "$repo/build/$build_arch/dynamic/dyntest" ::/bin/sh
	mcopy -o -i "$spec" "$inner" "::/arch/$profile.img"
}

run_pcat()
{
	local arch=pcat image="$work/pcat.img" inner="$work/i386.img"
	local log="$work/pcat.log" start
	zedbsd_test_require_command qemu-system-i386
	zedbsd_test_require_command mcopy
	start="$(zedbsd_test_utc_now)"
	"$repo/build.sh" hdd-image pcat
	prepare_pc_overlay "$repo/build/pcat/hdd-image.img" "$image" i386 pcat "$inner"
	qemu-system-i386 -M pc -cpu 486 -m 64M -accel tcg -vga std \
		-nic none -display none -serial none -monitor none -no-reboot \
		-no-shutdown -debugcon "file:$log" -global isa-debugcon.iobase=0xe9 \
		-drive "if=ide,format=raw,file=$image" >/dev/null 2>&1 &
	qemu_pid=$!
	wait_dynamic "$arch" "$image" "$log" "$start"
}

run_pc98()
{
	local arch=pc98 image="$work/pc98.img" inner="$work/pc98-i386.img"
	local log="$work/pc98.log" qmp="$work/pc98.qmp" start
	local qemu="${QEMU_PC98:-$HOME/qemu-pc98/build/qemu-system-i386}"
	local bios="${PC98_BIOS_DIR:-$HOME/qemu-pc98/roms/pc98bios}"
	test -x "$qemu"
	test -d "$bios"
	zedbsd_test_require_command mcopy
	start="$(zedbsd_test_utc_now)"
	"$repo/build.sh" hdd-image pc98
	prepare_pc_overlay "$repo/build/pc98/hdd-image.img" "$image" i386 pc98 "$inner"
	"$qemu" -M pc9821 -cpu 486 -m 64 -accel tcg -L "$bios" \
		-display none -serial none -monitor none -no-reboot -no-shutdown \
		-qmp "unix:$qmp,server=on,wait=off" \
		-debugcon "file:$log" -global isa-debugcon.iobase=0xe9 \
		-drive "if=ide,bus=0,unit=0,format=raw,file=$image" \
		>/dev/null 2>&1 &
	qemu_pid=$!
	wait_dynamic_screen "$arch" "$image" "$log" "$start" "$qmp"
}

run_amd64()
{
	local arch=amd64 image="$work/amd64.img" inner="$work/amd64-overlay.img"
	local log="$work/amd64.log" start
	zedbsd_test_require_command qemu-system-x86_64
	zedbsd_test_require_command mcopy
	start="$(zedbsd_test_utc_now)"
	"$repo/build.sh" hdd-image amd64
	prepare_pc_overlay "$repo/build/amd64/hdd-image.img" "$image" amd64 amd64 "$inner"
	qemu-system-x86_64 -M pc -cpu qemu64 -m 256M -smp 4 -accel tcg \
		-vga std -nic none -display none -serial none -monitor none \
		-no-reboot -no-shutdown -debugcon "file:$log" \
		-global isa-debugcon.iobase=0xe9 \
		-drive "if=ide,format=raw,file=$image" >/dev/null 2>&1 &
	qemu_pid=$!
	wait_dynamic "$arch" "$image" "$log" "$start"
}

run_arm64()
{
	local arch=arm64 image="$work/arm64.img" inner="$work/aarch64.img"
	local log="$work/arm64.log" spec start
	local dtb="$repo/vendor/raspberrypi-firmware/boot/bcm2711-rpi-4-b.dtb"
	zedbsd_test_require_command qemu-system-aarch64
	zedbsd_test_require_command mcopy
	start="$(zedbsd_test_utc_now)"
	"$repo/build.sh" hdd-image arm64
	cp --reflink=auto "$repo/build/arm64/hdd-image.img" "$image"
	spec="$image@@$((2048 * 512))"
	mcopy -i "$spec" ::/arch/aarch64.img "$inner"
	mcopy -o -i "$inner" "$repo/build/arm64/dynamic/dyntest" ::/bin/sh
	mcopy -o -i "$spec" "$inner" ::/arch/aarch64.img
	qemu-system-aarch64 -M raspi4b -smp 4 -m 2G \
		-kernel "$repo/build/arm64/VMUNIX.A64" \
		-drive "file=$image,if=sd,format=raw" -serial "file:$log" \
		-display none -monitor none -dtb "$dtb" -no-reboot \
		-no-shutdown >/dev/null 2>&1 &
	qemu_pid=$!
	wait_dynamic "$arch" "$image" "$log" "$start"
}

run_sparcv9()
{
	local arch=sparcv9 image="$work/sparcv9.img" log="$work/sparcv9.log"
	local prefix="${SPARCV9_PREFIX:-$HOME/opt/sparcv9}" start
	if test ! -x "$prefix/bin/sparc64-unknown-elf-gcc" &&
	    test -x "$HOME/opt/sparc64/bin/sparc64-unknown-elf-gcc"; then
		prefix="$HOME/opt/sparc64"
	fi
	zedbsd_test_require_command qemu-system-sparc64
	zedbsd_test_require_command mcopy
	start="$(zedbsd_test_utc_now)"
	make -C "$repo" ARCH=sparcv9 SPARCV9_PREFIX="$prefix" all \
		dynamic-userland-check -j"${ZEDBSD_TEST_JOBS:-4}"
	python3 "$repo/scripts/make-sparcv9-hdd-image.py" --force \
		--stage1 "$repo/build/sparcv9/boot/stage1.bin" \
		--stage2 "$repo/build/sparcv9/boot/stage2.bin" \
		--kernel "$repo/build/sparcv9/vmunix" \
		--shell "$repo/build/sparcv9/dynamic/dyntest" \
		--rtld "$repo/build/sparcv9/dynamic/ld.so" \
		--libc "$repo/build/sparcv9/dynamic/libc.so" \
		--tlstest "$repo/build/sparcv9/dynamic/tlstest.so" \
		--rpathdep "$repo/build/sparcv9/dynamic/alt/rpathdep.so" \
		--rpathtest "$repo/build/sparcv9/dynamic/rpathtest.so" \
		--verstest "$repo/build/sparcv9/dynamic/verstest.so" \
		--versuse "$repo/build/sparcv9/dynamic/versuse.so" \
		--dyntest "$repo/build/sparcv9/dynamic/dyntest" "$image"
	qemu-system-sparc64 -M sun4u -m 256M \
		-drive "file=$image,format=raw,if=ide" -nographic -no-reboot \
		-no-shutdown >"$log" 2>&1 &
	qemu_pid=$!
	wait_dynamic "$arch" "$image" "$log" "$start"
}

case "$selection" in
pcat) run_pcat ;;
pc98) run_pc98 ;;
amd64) run_amd64 ;;
arm64) run_arm64 ;;
sparcv9) run_sparcv9 ;;
all) run_pcat; run_pc98; run_amd64; run_arm64; run_sparcv9 ;;
*) echo "usage: $0 [all|pcat|pc98|amd64|arm64|sparcv9]" >&2; exit 2 ;;
esac

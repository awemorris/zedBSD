#!/usr/bin/env bash
# QEMU smoke tests for one image on PC/AT and PC-98.
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
set -euo pipefail

repo="$(cd "$(dirname "$0")/.." && pwd)"
build="$repo/build/pc-unified"
tmp="$(mktemp -d "${TMPDIR:-/tmp}/zedbsd-pc-unified.XXXXXX")"
trap 'rm -rf "$tmp"' EXIT

run_and_expect()
{
	local marker="$1" timeout_seconds="$2" log="$3"
	shift 3
	: >"$log"
	"$@" &
	local qemu_pid=$! deadline=$((SECONDS + timeout_seconds))
	while kill -0 "$qemu_pid" 2>/dev/null; do
		if grep -Fq "$marker" "$log"; then
			kill "$qemu_pid" 2>/dev/null || true
			wait "$qemu_pid" 2>/dev/null || true
			cat "$log"
			return 0
		fi
		if test "$SECONDS" -ge "$deadline"; then break; fi
		sleep 0.1
	done
	kill "$qemu_pid" 2>/dev/null || true
	wait "$qemu_pid" 2>/dev/null || true
	echo "Unified BIOS QEMU test failed: missing '$marker'" >&2
	cat "$log" >&2
	exit 1
}

make_image()
{
	local at_kernel="$1" image="$2" fragment="${3:-no}"
	local options=()
	if test "$fragment" = yes; then options+=(--fragment-kernels); fi
	python3 "$repo/scripts/make-pc-unified-hdd-image.py" --force \
		--stage0 "$build/stage0.bin" \
		--pc98-stage1 "$build/pc98-stage1.bin" \
		--pc98-stage2 "$build/pc98-stage2.bin" \
		--pcat-stage1 "$build/pcat-stage1.bin" \
		--pcat-stage2 "$build/pcat-stage2.bin" \
		--pc98-kernel "$repo/build/pc98/bootloader/payload32.elf" \
		--pcat-kernel "$at_kernel" \
		--amd64-kernel "$at_kernel" \
		--bootx64 "$repo/build/uefi/BOOTX64.EFI" \
		"${options[@]}" "$image"
}

qemu32="${QEMU_PCAT_I386:-$(command -v qemu-system-i386)}"
qemu64="${QEMU_PCAT_X86_64:-$(command -v qemu-system-x86_64)}"
qemu98="${QEMU_PC98:-/home/awe/qemu-pc98/build/qemu-system-i386}"
bios98="${PC98_BIOS_DIR:-/home/awe/qemu-pc98/roms/pc98bios}"
test -x "$qemu32" && test -x "$qemu64" && test -x "$qemu98"
test -d "$bios98"

make_image "$repo/build/pcat/bootloader/payload32.elf" "$tmp/dual32.img"
run_and_expect "P32 PASS" 8 "$tmp/at32.log" "$qemu32" \
	-M pc -m 64 -accel tcg -nic none -display none -serial none \
	-monitor none -no-reboot -snapshot -debugcon "file:$tmp/at32.log" \
	-global isa-debugcon.iobase=0xe9 \
	-drive "if=ide,format=raw,file=$tmp/dual32.img"
run_and_expect "N32 H8 PASS" 12 "$tmp/pc98-32.log" "$qemu98" \
	-M pc9821 -cpu 486 -m 64 -accel tcg -L "$bios98" -nic none \
	-display none -serial none -monitor none -no-reboot -snapshot \
	-debugcon "file:$tmp/pc98-32.log" -global isa-debugcon.iobase=0xe9 \
	-drive "if=ide,bus=0,unit=0,format=raw,file=$tmp/dual32.img"

make_image "$repo/build/pcat/bootloader/payload64.elf" "$tmp/dual64.img"
run_and_expect "P64 PASS" 8 "$tmp/at64.log" "$qemu64" \
	-M pc -cpu qemu64 -m 64 -accel tcg -nic none -display none \
	-serial none -monitor none -no-reboot -snapshot \
	-debugcon "file:$tmp/at64.log" -global isa-debugcon.iobase=0xe9 \
	-drive "if=ide,format=raw,file=$tmp/dual64.img"

make_image "$repo/build/pcat/bootloader/payload32.elf" \
	"$tmp/dual-fragmented.img" yes
run_and_expect "P32 PASS" 8 "$tmp/at-fragmented.log" "$qemu32" \
	-M pc -m 64 -accel tcg -nic none -display none -serial none \
	-monitor none -no-reboot -snapshot \
	-debugcon "file:$tmp/at-fragmented.log" -global isa-debugcon.iobase=0xe9 \
	-drive "if=ide,format=raw,file=$tmp/dual-fragmented.img"
run_and_expect "N32 H8 PASS" 12 "$tmp/pc98-fragmented.log" "$qemu98" \
	-M pc9821 -cpu 486 -m 64 -accel tcg -L "$bios98" -nic none \
	-display none -serial none -monitor none -no-reboot -snapshot \
	-debugcon "file:$tmp/pc98-fragmented.log" -global isa-debugcon.iobase=0xe9 \
	-drive "if=ide,bus=0,unit=0,format=raw,file=$tmp/dual-fragmented.img"

echo "Unified BIOS loader QEMU test: PASS"


#!/usr/bin/env bash
# QEMU smoke tests for the native BIOS loaders.
# Copyright (C) 2026 Awe Morris
# SPDX-License-Identifier: Zlib
set -euo pipefail

machine="${1:?usage: $0 pcat|pc98}"
repo="$(cd "$(dirname "$0")/.." && pwd)"
build="$repo/build/$machine"
tmp="$(mktemp -d "${TMPDIR:-/tmp}/zedbsd-bios-loader.XXXXXX")"
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
		if test "$SECONDS" -ge "$deadline"; then
			break
		fi
		sleep 0.1
	done
	kill "$qemu_pid" 2>/dev/null || true
	wait "$qemu_pid" 2>/dev/null || true
	if ! grep -Fq "$marker" "$log"; then
		echo "BIOS loader QEMU test failed: missing '$marker'" >&2
		cat "$log" >&2
		exit 1
	fi
	cat "$log"
}

make_image()
{
	local kernel="$1" image="$2" size_mib="${3:-129}" fragment="${4:-no}"
	local fragment_option=()
	if test "$fragment" = yes; then
		fragment_option=(--fragment-kernel)
	fi
	python3 "$repo/scripts/make-bios-hdd-image.py" --force \
		--machine "$machine" --stage1 "$build/bootloader/stage1.bin" \
		--stage2 "$build/bootloader/stage2.bin" --kernel "$kernel" \
		--size-mib "$size_mib" "${fragment_option[@]}" "$image"
	python3 "$repo/scripts/check-bios-hdd-image.py" --machine "$machine" \
		--kernel "$kernel" "$image"
}

case "$machine" in
pcat)
	qemu32="${QEMU_PCAT_I386:-$(command -v qemu-system-i386)}"
	qemu64="${QEMU_PCAT_X86_64:-$(command -v qemu-system-x86_64)}"
	for qemu in "$qemu32" "$qemu64"; do
		echo "QEMU=$qemu"
		"$qemu" --version | head -1
	done
	make_image "$build/bootloader/payload32.elf" "$tmp/payload32.img"
	run_and_expect "P32 PASS" 8 "$tmp/payload32.log" "$qemu32" \
		-M pc -m 64 -accel tcg -nic none -display none -serial none \
		-monitor none -no-reboot -snapshot -debugcon "file:$tmp/payload32.log" \
		-global isa-debugcon.iobase=0xe9 -drive "if=ide,format=raw,file=$tmp/payload32.img"
	make_image "$build/bootloader/payload32.elf" "$tmp/payload32-fragmented.img" 129 yes
	run_and_expect "P32 PASS" 8 "$tmp/payload32-fragmented.log" "$qemu32" \
		-M pc -m 64 -accel tcg -nic none -display none -serial none \
		-monitor none -no-reboot -snapshot \
		-debugcon "file:$tmp/payload32-fragmented.log" -global isa-debugcon.iobase=0xe9 \
		-drive "if=ide,format=raw,file=$tmp/payload32-fragmented.img"
	make_image "$build/bootloader/payload64.elf" "$tmp/payload64.img"
	run_and_expect "P64 PASS" 8 "$tmp/payload64.log" "$qemu64" \
		-M pc -cpu qemu64 -m 64 -accel tcg -nic none -display none -serial none \
		-monitor none -no-reboot -snapshot -debugcon "file:$tmp/payload64.log" \
		-global isa-debugcon.iobase=0xe9 -drive "if=ide,format=raw,file=$tmp/payload64.img"
	make_image "$build/bootloader/payload64.elf" "$tmp/payload64-fragmented.img" 129 yes
	run_and_expect "P64 PASS" 8 "$tmp/payload64-fragmented.log" "$qemu64" \
		-M pc -cpu qemu64 -m 64 -accel tcg -nic none -display none -serial none \
		-monitor none -no-reboot -snapshot \
		-debugcon "file:$tmp/payload64-fragmented.log" -global isa-debugcon.iobase=0xe9 \
		-drive "if=ide,format=raw,file=$tmp/payload64-fragmented.img"
	;;
pc98)
	qemu="${QEMU_PC98:-/home/awe/qemu-pc98/build/qemu-system-i386}"
	bios="${PC98_BIOS_DIR:-/home/awe/qemu-pc98/roms/pc98bios}"
	test -x "$qemu"
	test -d "$bios"
	echo "QEMU=$(readlink -f "$qemu")"
	stat -c 'QEMU_MTIME=%y' "$qemu"
	"$qemu" --version | head -1
	make_image "$build/bootloader/payload32.elf" "$tmp/payload32.img"
	run_and_expect "N32 H8 PASS" 12 "$tmp/payload32.log" "$qemu" \
		-M pc9821 -cpu 486 -m 64 -accel tcg -L "$bios" -nic none \
		-display none -serial none -monitor none -no-reboot -snapshot \
		-debugcon "file:$tmp/payload32.log" -global isa-debugcon.iobase=0xe9 \
		-drive "if=ide,bus=0,unit=0,format=raw,file=$tmp/payload32.img"
	make_image "$build/bootloader/payload32.elf" "$tmp/payload32-fragmented.img" 129 yes
	run_and_expect "N32 H8 PASS" 12 "$tmp/payload32-fragmented.log" "$qemu" \
		-M pc9821 -cpu 486 -m 64 -accel tcg -L "$bios" -nic none \
		-display none -serial none -monitor none -no-reboot -snapshot \
		-debugcon "file:$tmp/payload32-fragmented.log" -global isa-debugcon.iobase=0xe9 \
		-drive "if=ide,bus=0,unit=0,format=raw,file=$tmp/payload32-fragmented.img"
	make_image "$build/bootloader/payload32.elf" "$tmp/payload32-h4.img" 17
	run_and_expect "N32 H4 PASS" 12 "$tmp/payload32-h4.log" "$qemu" \
		-M pc9821 -cpu 486 -m 64 -accel tcg -L "$bios" -nic none \
		-display none -serial none -monitor none -no-reboot -snapshot \
		-debugcon "file:$tmp/payload32-h4.log" -global isa-debugcon.iobase=0xe9 \
		-drive "if=ide,bus=0,unit=0,format=raw,file=$tmp/payload32-h4.img"
	;;
*)
	echo "unsupported machine: $machine" >&2
	exit 2
	;;
esac

echo "BIOS loader QEMU test: PASS ($machine)"

#!/usr/bin/env bash
# amd64 kernel / ELF32 userland integration test on PC/AT.
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
set -euo pipefail

repo="$(cd "$(dirname "$0")/.." && pwd)"
source_image="${1:-$repo/build/amd64-pcat/hdd-image.img}"
fragmented_image="${2:-$repo/build/amd64-pcat/bios-hdd-image-fragmented.img}"
work="$(mktemp -d "${TMPDIR:-/tmp}/zedbsd-amd64-pcat.XXXXXX")"
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
offset=$((2048 * 512))

cp --reflink=auto "$source_image" "$work/test.img"
cp --reflink=auto "$fragmented_image" "$work/fragmented.img"
printf '%s\n' 'echo A64 USER32 PASS' 'halt' >"$work/zinit.rc"
for image in "$work/test.img" "$work/fragmented.img"; do
	mmd -i "$image@@$offset" ::/etc 2>/dev/null || true
	mcopy -o -i "$image@@$offset" "$work/zinit.rc" ::/etc/zinit.rc
done

qemu="${QEMU_PCAT_X86_64:-qemu-system-x86_64}"

run_qemu()
{
	local image="$1" memory="$2" video="$3" label="$4" extra_marker="$5"
	local log="$work/qemu-${label}.log" found=0
	"$qemu" -M pc -cpu qemu64 -m "$memory" -accel tcg -vga "$video" \
		-nic none -display none -serial none -monitor none \
		-no-reboot -no-shutdown -snapshot \
		-debugcon "file:$log" -global isa-debugcon.iobase=0xe9 \
		-drive "if=ide,format=raw,file=$image" >/dev/null 2>&1 &
	qemu_pid=$!
	for _ in $(seq 1 200); do
		if test -f "$log" && grep -Fq 'A64 USER32 PASS' "$log"; then
			found=1
			break
		fi
		if ! kill -0 "$qemu_pid" 2>/dev/null; then break; fi
		sleep 0.1
	done
	kill "$qemu_pid" 2>/dev/null || true
	wait "$qemu_pid" 2>/dev/null || true
	qemu_pid=
	if test "$found" -ne 1; then
		cat "$log" >&2
		echo "amd64 user marker timed out: $label" >&2
		exit 1
	fi
	for marker in 'A64 PAGING PASS' 'A64 IRQ READY' \
		'A64 TIMER TICK' 'boot: platform devices detected: 1' \
		'A64 USER32 PASS'; do
		if ! grep -Fq "$marker" "$log"; then
			cat "$log" >&2
			echo "missing amd64 marker ($label): $marker" >&2
			exit 1
		fi
	done
	if test -n "$extra_marker" && ! grep -Fq "$extra_marker" "$log"; then
		cat "$log" >&2
		echo "missing amd64 marker ($label): $extra_marker" >&2
		exit 1
	fi
	if grep -Fq 'fatal:' "$log"; then
		cat "$log" >&2
		echo "amd64 fatal error: $label" >&2
		exit 1
	fi
	echo "amd64 PC/AT QEMU: PASS ($label)"
}

for memory in 32 64 256; do
	run_qemu "$work/test.img" "$memory" std "${memory}-MiB" ''
done
run_qemu "$work/fragmented.img" 64 std fragmented-kernel ''
run_qemu "$work/test.img" 64 cirrus cirrus-vga 'graphics: PCI Cirrus'

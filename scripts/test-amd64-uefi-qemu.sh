#!/usr/bin/env bash
# OVMF -> amd64 kernel -> native PIIX IDE root integration test.
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
set -euo pipefail

repo="$(cd "$(dirname "$0")/.." && pwd)"
source_image="${1:-$repo/build/unified/hdd-image.img}"
ovmf_code="${OVMF_CODE:-/usr/share/OVMF/OVMF_CODE_4M.fd}"
ovmf_vars="${OVMF_VARS:-/usr/share/OVMF/OVMF_VARS_4M.fd}"
qemu="${QEMU_PCAT_X86_64:-qemu-system-x86_64}"
work="$(mktemp -d "${TMPDIR:-/tmp}/zedbsd-amd64-uefi.XXXXXX")"
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

test -f "$source_image"
test -f "$ovmf_code"
test -f "$ovmf_vars"
command -v "$qemu" >/dev/null

cp --reflink=auto "$source_image" "$work/test.img"
printf '%s\n' 'echo A64 UEFI USER32 PASS' 'halt' >"$work/zinit.rc"
offset=$((2048 * 512))
mcopy -i "$work/test.img@@$offset" ::/rootfs.x64 "$work/rootfs.x64"
mmd -i "$work/rootfs.x64" ::/etc 2>/dev/null || true
mcopy -o -i "$work/rootfs.x64" "$work/zinit.rc" ::/etc/zinit.rc
mcopy -o -i "$work/test.img@@$offset" "$work/rootfs.x64" ::/rootfs.x64

run_one()
{
	local memory="$1" log="$work/qemu-${memory}.log" found=0
	local vars="$work/vars-${memory}.fd"
	cp "$ovmf_vars" "$vars"
	"$qemu" -M pc -cpu qemu64 -m "$memory" -accel tcg \
		-nic none -display none -serial none -monitor none \
		-no-reboot -no-shutdown -snapshot \
		-debugcon "file:$log" -global isa-debugcon.iobase=0xe9 \
		-drive "if=pflash,format=raw,readonly=on,file=$ovmf_code" \
		-drive "if=pflash,format=raw,file=$vars" \
		-drive "if=ide,index=0,media=disk,format=raw,file=$work/test.img" \
		>/dev/null 2>&1 &
	qemu_pid=$!
	for _ in $(seq 1 300); do
		if test -f "$log" && grep -Fq 'A64 UEFI USER32 PASS' "$log"; then
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
		echo "amd64 UEFI user marker timed out: ${memory} MiB" >&2
		exit 1
	fi
	for marker in \
		'A64 UEFI ENTRY' 'A64 UEFI ELF' 'A64 UEFI EXIT' \
		'A64 PAGING PASS' 'A64 IRQ READY' 'ata: ide0' \
		'vfs: ide0 partition 1 start=2048' 'A64 UEFI USER32 PASS'
	do
		if ! grep -Fq "$marker" "$log"; then
			cat "$log" >&2
			echo "missing UEFI marker (${memory} MiB): $marker" >&2
			exit 1
		fi
	done
	if grep -Fq 'fatal:' "$log"; then
		cat "$log" >&2
		echo "amd64 UEFI fatal error: ${memory} MiB" >&2
		exit 1
	fi
	echo "amd64 UEFI QEMU: PASS (${memory} MiB)"
}

# Debian's 4-MiB OVMF build does not reach the boot manager with only 32 MiB.
# Exercise three usable sizes while keeping the kernel allocator below 1 GiB.
for memory in 64 128 256; do
	run_one "$memory"
done

echo "amd64 UEFI integration test: PASS"

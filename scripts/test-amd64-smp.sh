#!/usr/bin/env bash
# amd64 HAL-ready baseline across the supported fixed CPU counts.
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
set -euo pipefail

repo="$(cd "$(dirname "$0")/.." && pwd)"
qemu="${QEMU_PCAT_X86_64:-qemu-system-x86_64}"
work="$(mktemp -d "${TMPDIR:-/tmp}/zedbsd-amd64-smp.XXXXXX")"
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

"$repo/build.sh" hdd-image amd64
printf '%s\n' 'echo AMD64_SMP_BOOT_PASS' 'halt' >"$work/zinit.rc"

for cpus in 1 2 4 8; do
	image="$work/amd64-${cpus}.img"
	log="$work/amd64-${cpus}.log"
	cp --reflink=auto "$repo/build/amd64/hdd-image.img" "$image"
	rootfs="$work/rootfs-${cpus}.img"
	mcopy -i "$image@@$((2048 * 512))" ::/rootfs.img "$rootfs"
	mmd -i "$rootfs" ::/etc 2>/dev/null || true
	mcopy -o -i "$rootfs" "$work/zinit.rc" ::/etc/zinit.rc
	mcopy -o -i "$image@@$((2048 * 512))" "$rootfs" ::/rootfs.img
	"$qemu" -M pc -cpu qemu64 -smp "$cpus" -m 128M -accel tcg \
		-nic none -display none -serial none -monitor none -snapshot \
		-no-reboot -no-shutdown \
		-debugcon "file:$log" -global isa-debugcon.iobase=0xe9 \
		-drive "if=ide,format=raw,file=$image" >/dev/null 2>&1 &
	qemu_pid=$!
	found=0
	for _ in $(seq 1 300); do
		if test -f "$log" &&
		    grep -Fq 'AMD64_SMP_BOOT_PASS' "$log"; then
			found=1
			break
		fi
		if ! kill -0 "$qemu_pid" 2>/dev/null; then break; fi
		sleep 0.1
	done
	kill "$qemu_pid" 2>/dev/null || true
	wait "$qemu_pid" 2>/dev/null || true
	qemu_pid=
	if test "$found" -ne 1 ||
	    ! grep -Fq "boot: CPUs ready: $cpus" "$log" ||
	    grep -Fq 'fatal:' "$log"; then
		cat "$log" >&2
		echo "amd64 SMP baseline failed: -smp $cpus" >&2
		exit 1
	fi
	cp "$log" "$repo/build/amd64/amd64-smp-${cpus}.log"
	echo "amd64 SMP HAL-ready baseline: PASS (-smp $cpus)"
done

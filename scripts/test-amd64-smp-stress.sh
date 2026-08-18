#!/usr/bin/env bash
# Parallel mmap/filesystem/socket lifetime stress with resource baseline gate.
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
set -euo pipefail

repo="$(cd "$(dirname "$0")/.." && pwd)"
qemu="${QEMU_PCAT_X86_64:-qemu-system-x86_64}"
expected_events="${SMP_STRESS_EXPECT:-100008}"
work="$(mktemp -d "${TMPDIR:-/tmp}/zedbsd-amd64-stress.XXXXXX")"
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
"$repo/build.sh" SMP-STRESS.ELF amd64
image="$work/amd64-stress.img"
inner="$work/amd64-profile.img"
log="$work/amd64-stress.log"
spec="$image@@$((2048 * 512))"
cp --reflink=auto "$repo/build/amd64/hdd-image.img" "$image"
mcopy -i "$spec" ::/rootfs.img "$inner"
mcopy -o -i "$inner" "$repo/build/amd64/SMP-STRESS.ELF" ::/bin/sh
mcopy -o -i "$spec" "$inner" ::/rootfs.img

"$qemu" -M pc -cpu qemu64 -smp 8 -m 128M -accel tcg -nic none \
	-display none -serial none -monitor none -snapshot -no-reboot \
	-no-shutdown -debugcon "file:$log" -global isa-debugcon.iobase=0xe9 \
	-drive "if=ide,format=raw,file=$image" >/dev/null 2>&1 &
qemu_pid=$!

found=0
for _ in $(seq 1 1200); do
	if test -f "$log" && grep -Fq "AMD64_SMP_STRESS_PASS events=$expected_events" "$log"; then
		found=1
		break
	fi
	if test -f "$log" && grep -Fq 'SMP_STRESS_FAIL:' "$log"; then
		break
	fi
	if ! kill -0 "$qemu_pid" 2>/dev/null; then break; fi
	sleep 0.1
done
kill "$qemu_pid" 2>/dev/null || true
wait "$qemu_pid" 2>/dev/null || true
qemu_pid=

if test "$found" -ne 1 || grep -Fq 'fatal:' "$log"; then
	cat "$log" >&2
	echo 'amd64 SMP resource stress failed' >&2
	exit 1
fi
cp "$log" "$repo/build/amd64/amd64-smp-stress.log"
echo 'amd64 SMP mmap/fs/network resource stress: PASS (100008 events)'

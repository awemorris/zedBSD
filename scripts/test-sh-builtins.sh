#!/usr/bin/env bash
# Exercise the ring-3 /bin/sh debug builtins against a writable FAT16 image.
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
set -euo pipefail

arch="${1:?usage: $0 pcat|pc98}"
repo="$(cd "$(dirname "$0")/.." && pwd)"
build="$repo/build/$arch"
work="$(mktemp -d "${TMPDIR:-/tmp}/zedbsd-sh-builtins-$arch.XXXXXX")"
trap 'rm -rf "$work"' EXIT
offset=$((2048 * 512))

case "$arch" in
pcat|pc98) ;;
*) echo "unsupported platform: $arch" >&2; exit 2 ;;
esac

"$repo/build.sh" bios-hdd-image "$arch"
cp --reflink=auto "$build/bios-hdd-image.img" "$work/test.img"
printf '%s\n' 'zedBSD shell builtin copy fixture' >"$work/source.txt"
printf '%s\n' \
	'pwd' \
	'ls /' \
	'ls -l /bin' \
	'cat /source.txt' \
	'cp /source.txt /copy.txt' \
	'stat /copy.txt' \
	'touch /touch.txt' \
	'cd /bin' \
	'pwd' \
	'ls' \
	'cp ../copy.txt ../copy2.txt' \
	'cp /source.txt /bin/overlay.txt' \
	'stat /bin/overlay.txt' \
	'clear' \
	'true' \
	'echo SH_BUILTINS_PASS' \
	'halt' >"$work/zinit.rc"

mmd -i "$work/test.img@@$offset" ::/etc 2>/dev/null || true
mcopy -i "$work/test.img@@$offset" "$work/source.txt" ::/source.txt
mcopy -i "$work/test.img@@$offset" "$work/zinit.rc" ::/etc/zinit.rc

case "$arch" in
pcat)
	qemu="${QEMU_PCAT_I386:-qemu-system-i386}"
	timeout 25 "$qemu" -M pc -cpu 486 -m 64M -display none \
		-serial none -monitor none -no-reboot -no-shutdown \
		-debugcon "file:$work/debug.log" -global isa-debugcon.iobase=0xe9 \
		-drive "if=ide,index=0,media=disk,format=raw,file=$work/test.img" \
		>/dev/null 2>&1 || test "$?" -eq 124
	for marker in 'zedBSD shell builtin copy fixture' SH_BUILTINS_PASS; do
		if ! grep -Fq "$marker" "$work/debug.log"; then
			cat "$work/debug.log" >&2
			echo "missing shell test marker: $marker" >&2
			exit 1
		fi
	done
	grep -Fq '/bin' "$work/debug.log"
	grep -Fq 'type=regular' "$work/debug.log"
	if grep -Fq 'command failed' "$work/debug.log"; then
		cat "$work/debug.log" >&2
		echo "a shell builtin failed" >&2
		exit 1
	fi
	;;
pc98)
	qemu="${QEMU_PC98:-/home/awe/qemu-pc98/build/qemu-system-i386}"
	bios="${PC98_BIOS_DIR:-/home/awe/qemu-pc98/roms/pc98bios}"
	test -x "$qemu" && test -d "$bios"
	timeout 25 "$qemu" -M pc9821 -cpu 486 -m 64 -accel tcg -L "$bios" \
		-nic none -display none -serial none -monitor none -no-reboot \
		-drive "if=ide,bus=0,unit=0,format=raw,file=$work/test.img" \
		>/dev/null 2>&1 || test "$?" -eq 124
	;;
esac

for name in copy.txt copy2.txt; do
	mcopy -i "$work/test.img@@$offset" "::/$name" "$work/$name"
	cmp "$work/source.txt" "$work/$name"
done
mcopy -i "$work/test.img@@$offset" ::/touch.txt "$work/touch.txt"
test ! -s "$work/touch.txt"

# /bin is a direct overlay: the write must persist inside the selected inner
# FAT image and must never leak into the outer lower /bin directory.
if mdir -i "$work/test.img@@$offset" ::/bin/overlay.txt >/dev/null 2>&1; then
	echo "overlay write leaked into the outer FAT /bin" >&2
	exit 1
fi
mcopy -i "$work/test.img@@$offset" ::/arch/i386.img "$work/i386.img"
mcopy -i "$work/i386.img" ::/bin/overlay.txt "$work/overlay.txt"
cmp "$work/source.txt" "$work/overlay.txt"

echo "zedBSD /bin/sh builtin QEMU test: PASS ($arch)"

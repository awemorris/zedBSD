#!/usr/bin/env bash
# QEMU integration test for LGY-98, IPv4, UDP, and active-open TCP.
# Copyright (C) 2026 Awe Morris
# SPDX-License-Identifier: Zlib
set -euo pipefail

repo="$(cd "$(dirname "$0")/.." && pwd)"
build="$repo/build/pc98"
qemu="${QEMU_PC98:-/home/awe/qemu-pc98/build/qemu-system-i386}"
bios="${PC98_BIOS_DIR:-/home/awe/qemu-pc98/roms/pc98bios}"
tmp="$(mktemp -d "${TMPDIR:-/tmp}/zedbsd-network.XXXXXX")"
peer_pid=""
qemu_pid=""

cleanup()
{
	test -z "$qemu_pid" || kill "$qemu_pid" 2>/dev/null || true
	test -z "$peer_pid" || kill "$peer_pid" 2>/dev/null || true
	test -z "$qemu_pid" || wait "$qemu_pid" 2>/dev/null || true
	test -z "$peer_pid" || wait "$peer_pid" 2>/dev/null || true
	rm -rf "$tmp"
}
trap cleanup EXIT

test -x "$qemu"
test -d "$bios"
cp --reflink=auto "$repo/build/arch-images/i386.img" "$tmp/i386.img"
mcopy -o -i "$tmp/i386.img" "$build/bin/nettest" ::/bin/sh
python3 "$repo/scripts/make-bios-hdd-image.py" --force \
	--machine pc98 --stage1 "$build/bootloader/stage1.bin" \
	--stage2 "$build/bootloader/stage2.bin" --kernel "$build/vmunix" \
	--arch-profile i386 --arch-image "$tmp/i386.img" "$tmp/network.img"

python3 "$repo/scripts/network-test-peer.py" --log "$tmp/peer.log" &
peer_pid=$!
for _ in $(seq 1 50); do
	grep -Fq "PEER READY" "$tmp/peer.log" 2>/dev/null && break
	sleep 0.1
done
grep -Fq "PEER READY" "$tmp/peer.log"

"$qemu" -M pc9821 -cpu 486 -m 64 -accel tcg -L "$bios" \
	-drive "if=ide,bus=0,unit=0,format=raw,file=$tmp/network.img" \
	-snapshot -netdev user,id=net0 -device pc98-lgy98,netdev=net0 \
	-display none -serial none -monitor none -no-reboot \
	-debugcon "file:$tmp/debug.log" -global isa-debugcon.iobase=0xe9 &
qemu_pid=$!

deadline=$((SECONDS + 25))
while kill -0 "$qemu_pid" 2>/dev/null && test "$SECONDS" -lt "$deadline"; do
	if grep -Fq "UDP PASS" "$tmp/peer.log" &&
	   grep -Fq "TCP PASS" "$tmp/peer.log"; then
		grep -Fq "net: LGY-98 registered as ne0" "$tmp/debug.log"
		cat "$tmp/peer.log"
		echo "PC-98 network QEMU test: PASS"
		exit 0
	fi
	sleep 0.1
done

echo "PC-98 network QEMU test failed" >&2
cat "$tmp/peer.log" >&2 || true
cat "$tmp/debug.log" >&2 || true
exit 1

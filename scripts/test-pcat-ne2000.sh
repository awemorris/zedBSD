#!/usr/bin/env bash
# QEMU integration test for the PC/AT ISA NE2000 network path.
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
set -euo pipefail

repo="$(cd "$(dirname "$0")/.." && pwd)"
arch="${1:-}"
case "$arch" in
pcat)
	qemu="${QEMU_PCAT_I386:-qemu-system-i386}"
	cpu=486
	;;
amd64)
	qemu="${QEMU_PCAT_X86_64:-qemu-system-x86_64}"
	cpu=qemu64
	;;
*)
	echo "usage: $0 {pcat|amd64}" >&2
	exit 2
	;;
esac
build="$repo/build/$arch"
tmp="$(mktemp -d "${TMPDIR:-/tmp}/zedbsd-ne2000.XXXXXX")"
peer_pid=""
qemu_pid=""

cleanup()
{
	test -z "$qemu_pid" || kill "$qemu_pid" 2>/dev/null || true
	test -z "$peer_pid" || kill "$peer_pid" 2>/dev/null || true
	test -z "$qemu_pid" || wait "$qemu_pid" 2>/dev/null || true
	test -z "$peer_pid" || wait "$peer_pid" 2>/dev/null || true
	rm -rf -- "$tmp"
}
trap cleanup EXIT INT TERM

command -v "$qemu" >/dev/null || {
	echo "QEMU not found: $qemu" >&2
	exit 1
}

python3 "$repo/scripts/make-bios-hdd-image.py" --force \
	--machine pcat --stage1 "$build/bootloader/stage1.bin" \
	--stage2 "$build/bootloader/stage2.bin" --kernel "$build/vmunix" \
	--shell "$build/bin/nettest" "$tmp/network.img"

python3 "$repo/scripts/network-test-peer.py" --log "$tmp/peer.log" &
peer_pid=$!
for _ in $(seq 1 50); do
	grep -Fq "PEER READY" "$tmp/peer.log" 2>/dev/null && break
	sleep 0.1
done
grep -Fq "PEER READY" "$tmp/peer.log"

"$qemu" -M pc -cpu "$cpu" -m 64 -accel tcg \
	-drive "if=ide,format=raw,file=$tmp/network.img" -snapshot \
	-netdev user,id=net0 \
	-device ne2k_isa,netdev=net0,iobase=0x300,irq=10,mac=52:54:00:12:34:56 \
	-display none -serial none -monitor none -no-reboot -no-shutdown \
	-debugcon "file:$tmp/debug.log" -global isa-debugcon.iobase=0xe9 \
	>/dev/null 2>&1 &
qemu_pid=$!

deadline=$((SECONDS + 30))
while kill -0 "$qemu_pid" 2>/dev/null && test "$SECONDS" -lt "$deadline"; do
	if grep -Fq "UDP PASS" "$tmp/peer.log" &&
	   grep -Fq "TCP PASS" "$tmp/peer.log"; then
		break
	fi
	sleep 0.1
done

failed=0
for marker in \
	"net: ISA NE2000 at 0x300 irq 10 registered as ne0" \
	"nettest: ICMP echo reply"; do
	if ! grep -Fq "$marker" "$tmp/debug.log"; then
		echo "missing guest marker: $marker" >&2
		failed=1
	fi
done
for marker in "UDP PASS" "TCP PASS"; do
	if ! grep -Fq "$marker" "$tmp/peer.log"; then
		echo "missing peer marker: $marker" >&2
		failed=1
	fi
done
for marker in "fatal:" "VFS initialization failed" \
	"configuring ne0 failed"; do
	if grep -Fq "$marker" "$tmp/debug.log"; then
		echo "unexpected guest marker: $marker" >&2
		failed=1
	fi
done
if test "$failed" -ne 0; then
	echo "PC/AT ISA NE2000 QEMU test failed ($arch)" >&2
	echo "--- peer log ---" >&2
	cat "$tmp/peer.log" >&2 || true
	echo "--- guest log ---" >&2
	cat "$tmp/debug.log" >&2 || true
	exit 1
fi

cat "$tmp/peer.log"
echo "PC/AT ISA NE2000 QEMU test: PASS ($arch)"

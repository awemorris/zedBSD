#!/bin/sh
# On the Pi 5, as root: saves the USB HID report ring (/dev/usbhid-ring, kernel booted with usbhid.ring=1) with the
# session's log, the kernel's log and the wall clock, for ring-decode.py.  Run it soon after a click failed: the ring
# keeps the newest 131072 reports (some 17 minutes of a mouse moving at 125 reports a second).
# usage: sh ring-save.sh [DIR]       (default /root/bug412/UTC-STAMP; prints SAVED DIR records=N)
# The files hold every key typed on every USB keyboard while the ring ran: they are root's (umask 077).
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
set -u
umask 077
dir=${1:-/root/bug412/$(date -u +%Y%m%d-%H%M%S)}
[ -c /dev/usbhid-ring ] || { echo "FAIL no /dev/usbhid-ring (boot with usbhid.ring=1)"; exit 1; }
mkdir -p "$dir" || exit 1
dd if=/dev/usbhid-ring of="$dir/ring.bin" bs=65536 2> "$dir/dd.txt" || { echo "FAIL reading the ring"; exit 1; }
saved=$(date +%s)
cp /run/user/1000/session.log "$dir/session.log" 2> /dev/null || echo "no session.log" > "$dir/session.log"
dmesg > "$dir/dmesg.txt" 2>&1
bytes=$(wc -c < "$dir/ring.bin" | tr -d ' ')
{
	echo "saved_epoch=$saved"
	echo "saved_utc=$(date -u)"
	echo "uname=$(uname -v)"
	echo "cmdline=$(cat /boot/boot0/cmdline.txt 2> /dev/null)"
	echo "ring_bytes=$bytes"
} > "$dir/info.txt"
sync
echo "SAVED $dir records=$((bytes / 64))"

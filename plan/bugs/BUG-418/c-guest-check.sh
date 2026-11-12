#!/bin/bash
# zedBSD: an amd64 image's packaged data, looked at in the running guest.
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
#
#   c-guest-check.sh IMAGE OUTPUT
#
# Boots IMAGE (an x86_64 uefi disk built with CONFIG_PCAT_SERIAL_MIRROR=y)
# from NVMe through OVMF, logs in as root on the serial line
# (plan/tools/guest/serial.py) and lists the data files the packages put in
# the image.  The answers go to OUTPUT/guest.txt and the verdict, PASS or
# FAIL, is the last line printed.  The emulator's work files stay in
# OUTPUT/work for the cleanup.
set -u
image=$(readlink -f -- "$1")
output=$2
root=$(cd "$(dirname "$0")/../../.." && pwd)
. "$root/plan/tools/guest/qemu-accel.sh"
serial_py=$root/plan/tools/guest/serial.py

mkdir -p "$output/work"
work=$(mktemp -d "$output/work/run.XXXXXX")
qemu-img create -q -f qcow2 -F raw -b "$image" "$work/disk.qcow2"
cp -- /usr/share/OVMF/OVMF_VARS_4M.fd "$work/vars.fd"

read -r -a accel <<<"$(qemu_accel_args max)"
timeout 900 qemu-system-x86_64 -machine q35 -m 8G -smp 4 "${accel[@]}" \
	-drive "if=pflash,format=raw,readonly=on,file=/usr/share/OVMF/OVMF_CODE_4M.fd" \
	-drive "if=pflash,format=raw,file=$work/vars.fd" \
	-drive "if=none,id=boot,file=$work/disk.qcow2,format=qcow2" \
	-device nvme,serial=zedbsd-boot,drive=boot,bootindex=1 \
	-vga std -display none -no-reboot \
	-serial "unix:$work/serial.sock,server,nowait" \
	-qmp "unix:$work/qmp.sock,server,nowait" >"$work/qemu.out" 2>&1 &
pid=$!
trap 'kill "$pid" 2>/dev/null; wait "$pid" 2>/dev/null' EXIT

for _ in $(seq 60); do
	test -S "$work/serial.sock" && break
	sleep 1
done

verdict=PASS
{
	timeout 400 python3 -I "$serial_py" --socket "$work/serial.sock" --timeout 300 expect 'login: ' >/dev/null || verdict=FAIL
	timeout 200 python3 -I "$serial_py" --socket "$work/serial.sock" login || verdict=FAIL
	# Each file the packages' data names, its mode, owner and size.
	for file in /etc/ssl/cert.pem \
		/usr/share/fonts/keiland-emoji.ttf \
		/usr/share/keiland/hand/hershey.txt \
		/usr/share/keiland/ime/ja/SKK-JISYO.ja \
		/usr/share/keiland/ime/skk/SKK-JISYO.X \
		/usr/share/keiland/ime/skk/SKK-JISYO.remacs \
		/usr/share/remacs/skkjisyo.dic \
		/usr/share/remacs/skkjisyo-x.dic \
		/usr/share/keiland/locale/ja/files.tr \
		/usr/share/licenses/ca-certificates/MPL-2.0.txt; do
		echo "== $file"
		timeout 120 python3 -I "$serial_py" --socket "$work/serial.sock" \
			run "ls -l $file" || verdict=FAIL
	done
	# The roots are certificates a user (not only root) can read.
	echo "== certificates read by user"
	timeout 120 python3 -I "$serial_py" --socket "$work/serial.sock" \
		run "su user -c 'grep -c BEGIN /etc/ssl/cert.pem'" || verdict=FAIL
} >"$output/guest.txt" 2>&1
timeout 60 python3 -I "$serial_py" --socket "$work/serial.sock" run poweroff >/dev/null 2>&1
sync
echo "c-guest-check: $verdict"

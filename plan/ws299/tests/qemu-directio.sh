#!/bin/sh
# ws299-p010: QEMU check of O_DIRECT on block devices.  Beside the NVMe boot disk a blank 1 GiB NVMe (512-byte blocks,
# page lists in place) and a blank 256 MiB AHCI disk (no page lists: every piece goes through staging).  The guest
# runs "directio check" (plan/ws299/tests/directio.c, put in the image as /root/directio) on each, then a direct
# write of 256 MiB of the NVMe disk and direct reads of it in 4 MiB and 1 MiB requests, and prints the I/O counts.
#   sh plan/ws299/tests/qemu-directio.sh IMAGE
# IMAGE: plan/tools/posix/config-amd64-serial.mk with ZEDBSD_EXTRA_FILES='--file /root/directio=OUT/directio-zedbsd'
# (OUT from build-randread.sh).  W is ${WS299_RUNS:-build/tmp}/qemu-directio (a fresh run each time).  The last line is
# "qemu-directio: PASS|FAIL ...".
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
set -u
. plan/tools/guest/qemu-accel.sh
. plan/tools/fresh-out.sh
IMAGE=${1:?image}
mkdir -p "${WS299_RUNS:-build/tmp}"; fresh_out "${WS299_RUNS:-build/tmp}"/qemu-directio
W=${WS299_RUNS:-build/tmp}/qemu-directio
truncate -s 1G "$W/nvme.img"
truncate -s 256M "$W/ahci.img"
cp /usr/share/OVMF/OVMF_VARS_4M.fd "$W/vars.fd"
timeout 1800 qemu-system-x86_64 -machine q35 -m 4G -smp 4 $(qemu_accel_args max) \
  -drive "if=pflash,format=raw,readonly=on,file=/usr/share/OVMF/OVMF_CODE_4M.fd" \
  -drive "if=pflash,format=raw,file=$W/vars.fd" \
  -drive "if=none,id=boot,file=$IMAGE,format=raw,snapshot=on" \
  -device nvme,serial=zedbsd-boot,drive=boot,bootindex=1 \
  -drive "if=none,id=d1,file=$W/nvme.img,format=raw" \
  -device nvme,serial=zedbsd-direct,drive=d1 \
  -drive "if=none,id=d2,file=$W/ahci.img,format=raw" \
  -device ahci,id=ahci -device ide-hd,drive=d2,bus=ahci.0 \
  -vga std -display none -no-reboot \
  -serial "unix:$W/serial.sock,server=on,wait=off" > "$W/qemu.log" 2>&1 &
pid=$!
trap 'kill $pid 2> /dev/null' EXIT
i=0
while [ ! -S "$W/serial.sock" ] && [ $i -lt 600 ]; do sleep 0.1; i=$((i + 1)); done
S="python3 plan/tools/guest/serial.py --socket $W/serial.sock --timeout 600"
$S expect 'login: ' > /dev/null || { echo "qemu-directio: FAIL no login"; exit 1; }
$S login > /dev/null
out=$W/guest.txt
: > "$out"
for command in 'ls /dev/nvme1n1 /dev/sd*' \
	'/root/directio check /dev/nvme1n1 /root/direct-file 2>&1' \
	'/root/directio check $(ls /dev/sd? | head -1) /root/direct-file2 2>&1' \
	'/root/directio write /dev/nvme1n1 256 1024 64 && /root/directio read /dev/nvme1n1 256 4096 64' \
	'/root/directio read /dev/nvme1n1 256 1024 64' \
	'sysctl vfs.io.stats 2>&1 | head -60'; do
	echo "guest\$ $command" | tee -a "$out"
	$S run "$command" | tee -a "$out"
done
tr -d '\r' < "$out" > "$out.clean"
passes=$(grep -c '^directio check: PASS' "$out.clean")
fails=$(grep -c '^FAIL ' "$out.clean")
rates=$(grep -c '^directio \(read\|write\) mib=256' "$out.clean")
if [ "$passes" -eq 2 ] && [ "$fails" -eq 0 ] && [ "$rates" -eq 3 ]; then
	echo "qemu-directio: PASS (2 checks, 3 transfers)"
else
	echo "qemu-directio: FAIL ($passes checks passed, $fails failures, $rates transfers)"
fi

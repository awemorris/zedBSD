#!/bin/sh
# ws299-p003: QEMU check of the NVMe page-list and bounce paths on namespaces of other shapes.  Beside the boot disk
# two blank NVMe disks of 512-byte blocks: nvme1 with MDTS 9 (2 MiB commands, a 511-entry PRP list), nvme2 with
# MDTS 3 (32 KiB commands, so a request splits into many).  The guest (8 GiB, so buffer-cache pages
# lie both below and above 4 GiB) writes the same 200 MiB of random data raw to each with 1 MiB and 64 KiB writes,
# reads it back the same ways, compares checksums with the source, and prints the kernel's page-list counts.
#   sh plan/ws299/tests/qemu-nvme-shapes.sh IMAGE         (IMAGE built with CONFIG_PCAT_SERIAL_MIRROR=y)
# W is ${WS299_RUNS:-build/tmp}/qemu-nvme-shapes (a fresh run each time).  The last line is "qemu-nvme-shapes: PASS|FAIL ...".
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
set -u
. plan/tools/guest/qemu-accel.sh
. plan/tools/fresh-out.sh
IMAGE=${1:?image}
mkdir -p "${WS299_RUNS:-build/tmp}"; fresh_out "${WS299_RUNS:-build/tmp}"/qemu-nvme-shapes
W=${WS299_RUNS:-build/tmp}/qemu-nvme-shapes
truncate -s 1G "$W/d4k.img"
truncate -s 1G "$W/d512.img"
cp /usr/share/OVMF/OVMF_VARS_4M.fd "$W/vars.fd"
qemu-system-x86_64 -machine q35 -m 8G -smp 4 $(qemu_accel_args max) \
  -drive "if=pflash,format=raw,readonly=on,file=/usr/share/OVMF/OVMF_CODE_4M.fd" \
  -drive "if=pflash,format=raw,file=$W/vars.fd" \
  -drive "if=none,id=boot,file=$IMAGE,format=raw,snapshot=on" \
  -device nvme,serial=zedbsd-boot,drive=boot,bootindex=1 \
  -drive "if=none,id=d4k,file=$W/d4k.img,format=raw" \
  -device nvme,serial=zedbsd-big,mdts=9,drive=d4k \
  -drive "if=none,id=d512,file=$W/d512.img,format=raw" \
  -device nvme,serial=zedbsd-512,mdts=3,drive=d512 \
  -vga std -display none -no-reboot \
  -serial "unix:$W/serial.sock,server=on,wait=off" > "$W/qemu.log" 2>&1 &
pid=$!
trap 'kill $pid 2> /dev/null' EXIT
i=0
while [ ! -S "$W/serial.sock" ] && [ $i -lt 600 ]; do sleep 0.1; i=$((i + 1)); done
S="python3 plan/tools/guest/serial.py --socket $W/serial.sock --timeout 900"
$S expect 'login: ' > /dev/null || { echo "qemu-nvme-shapes: FAIL no login"; exit 1; }
$S login > /dev/null
out=$W/guest.txt
: > "$out"
for command in 'dd if=/dev/urandom of=/root/src.bin bs=1024k count=200 2> /dev/null; cksum < /root/src.bin' \
	'dd if=/root/src.bin of=/dev/nvme1n1 bs=1024k 2> /dev/null; dd if=/dev/nvme1n1 bs=1024k count=200 2> /dev/null | cksum' \
	'dd if=/dev/nvme1n1 bs=64k count=3200 2> /dev/null | cksum' \
	'dd if=/root/src.bin of=/dev/nvme2n1 bs=64k 2> /dev/null; dd if=/dev/nvme2n1 bs=1024k count=200 2> /dev/null | cksum' \
	'dd if=/dev/nvme2n1 bs=64k count=3200 2> /dev/null | cksum' \
	'dmesg | grep -E "nvme: (/dev|page-list|no memory)"'; do
	echo "guest\$ $command" | tee -a "$out"
	$S run "$command" | tee -a "$out"
done
tr -d '\r' < "$out" > "$out.clean"
sums=$(grep -E '^[0-9]+ 209715200$' "$out.clean" | sort -u | wc -l)
count=$(grep -cE '^[0-9]+ 209715200$' "$out.clean")
if [ "$count" -eq 5 ] && [ "$sums" -eq 1 ]; then
	echo "qemu-nvme-shapes: PASS (5 checksums equal)"
else
	echo "qemu-nvme-shapes: FAIL ($count checksums, $sums distinct)"
fi

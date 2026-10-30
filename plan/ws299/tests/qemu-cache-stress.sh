#!/bin/sh
# ws299-p004: QEMU stress of the buffer cache at its cap (IMAGE built with config-qemu-cache128.mk: a 128 MiB cache).
# Beside the boot disk a 1 GiB NVMe with an empty MBR.  The guest writes 200 MiB of random data at 1 MiB and at
# 300 MiB, drops the cache (diskpart reload), then at once: four raw readers of the two regions (1 MiB and 64 KiB
# reads), a writer of 128 MiB to a file on the root UFS with a sync, and partition reloads every second; then it
# checks every reader's checksum against the source, the file's checksum after a second reload, and the raw regions
# again.  Reloads refused while readers hold the disk open are expected (EBUSY) and only counted.
#   sh plan/ws299/tests/qemu-cache-stress.sh IMAGE
# W is ${WS299_RUNS:-build/tmp}/qemu-cache-stress (a fresh run each time).  The last line is "qemu-cache-stress: PASS|FAIL ...".
# Every guest command line stays short (the console line is limited), so the concurrent part is written to scripts.
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
set -u
. plan/tools/guest/qemu-accel.sh
. plan/tools/fresh-out.sh
IMAGE=${1:?image}
mkdir -p "${WS299_RUNS:-build/tmp}"; fresh_out "${WS299_RUNS:-build/tmp}"/qemu-cache-stress
W=${WS299_RUNS:-build/tmp}/qemu-cache-stress
truncate -s 1G "$W/d.img"
printf '\125\252' | dd of="$W/d.img" bs=1 seek=510 conv=notrunc 2> /dev/null
cp /usr/share/OVMF/OVMF_VARS_4M.fd "$W/vars.fd"
timeout 2400 qemu-system-x86_64 -name p1-cache-stress -machine q35 -m 4G -smp 4 $(qemu_accel_args max) \
  -drive "if=pflash,format=raw,readonly=on,file=/usr/share/OVMF/OVMF_CODE_4M.fd" \
  -drive "if=pflash,format=raw,file=$W/vars.fd" \
  -drive "if=none,id=boot,file=$IMAGE,format=raw,snapshot=on" \
  -device nvme,serial=zedbsd-boot,drive=boot,bootindex=1 \
  -drive "if=none,id=d,file=$W/d.img,format=raw" \
  -device nvme,serial=zedbsd-stress,drive=d \
  -vga std -display none -no-reboot \
  -serial "unix:$W/serial.sock,server=on,wait=off" \
  -gdb "tcp:127.0.0.1:${GDB_PORT:-22212}" > "$W/qemu.log" 2>&1 &
pid=$!
trap 'kill $pid 2> /dev/null' EXIT
i=0
while [ ! -S "$W/serial.sock" ] && [ $i -lt 600 ]; do sleep 0.1; i=$((i + 1)); done
S="python3 plan/tools/guest/serial.py --socket $W/serial.sock --timeout 1500"
$S expect 'login: ' > /dev/null || { echo "qemu-cache-stress: FAIL no login"; exit 1; }
$S login > /dev/null
out=$W/guest.txt
: > "$out"
D=/dev/nvme1n1
for command in \
	"sysctl vfs.bufcache.max_bytes; dd if=/dev/urandom of=/root/src.bin bs=1024k count=200 2> /dev/null; cksum < /root/src.bin" \
	"dd if=/root/src.bin of=$D bs=1024k seek=1 conv=notrunc 2> /dev/null; dd if=/root/src.bin of=$D bs=1024k seek=300 conv=notrunc 2> /dev/null; diskpart reload $D > /dev/null; echo reload=\$?" \
	"dd if=/dev/urandom of=/root/w.src bs=1024k count=128 2> /dev/null; cksum < /root/w.src | sed 's/^/file /'" \
	"echo 'dd if=$D bs=1024k skip=1 count=200 2> /dev/null | cksum > /tmp/c1' > /tmp/r1.sh; echo 'dd if=$D bs=64k skip=4800 count=3200 2> /dev/null | cksum > /tmp/c2' > /tmp/r2.sh" \
	"echo 'dd if=$D bs=1024k skip=300 count=200 2> /dev/null | cksum > /tmp/c3' > /tmp/r3.sh; echo 'dd if=$D bs=64k skip=16 count=3200 2> /dev/null | cksum > /tmp/c4' > /tmp/r4.sh" \
	"echo 'dd if=/root/w.src of=/root/w.dst bs=1024k 2> /dev/null; sync' > /tmp/w.sh" \
	"echo 'r=0; n=0; while [ \$n -lt 8 ]; do sleep 1; diskpart reload $D > /dev/null 2>&1 || r=\$((r + 1)); n=\$((n + 1)); done; echo \"reload busy \$r of 8\"' > /tmp/l.sh" \
	"sh /tmp/r1.sh & sh /tmp/r2.sh & sh /tmp/r3.sh & sh /tmp/r4.sh & sh /tmp/w.sh & sh /tmp/l.sh; wait; cat /tmp/c1 /tmp/c2 /tmp/c3 /tmp/c4" \
	"diskpart reload $D > /dev/null; echo reload=\$?; cksum < /root/w.dst | sed 's/^/file /'" \
	"dd if=$D bs=1024k skip=1 count=200 2> /dev/null | cksum; dd if=$D bs=4k skip=76800 count=51200 2> /dev/null | cksum" \
	"sysctl vfs.bufcache.stats; dmesg | grep 'buf: readahead' | tail -2"; do
	echo "guest\$ $command" | tee -a "$out"
	$S run "$command" | tee -a "$out"
done
tr -d '\r' < "$out" > "$out.clean"
src=$(grep -E '^[0-9]+ 209715200$' "$out.clean" | head -1)
same=$(grep -cxF "$src" "$out.clean")
files=$(grep -E '^file [0-9]+ 134217728$' "$out.clean" | sort -u | wc -l)
filecount=$(grep -cE '^file [0-9]+ 134217728$' "$out.clean")
reloads=$(grep -c '^reload=0$' "$out.clean")
# src.bin's own checksum, four concurrent readers and two reads at the end: seven.
if [ "$same" -eq 7 ] && [ "$filecount" -eq 2 ] && [ "$files" -eq 1 ] && [ "$reloads" -eq 2 ]; then
	echo "qemu-cache-stress: PASS (7 equal raw checksums, file intact, $(grep -o 'reload busy [0-9]* of 8' "$out.clean"))"
else
	echo "qemu-cache-stress: FAIL (raw $same/7, file $filecount/$files, reloads ok $reloads/2)"
fi

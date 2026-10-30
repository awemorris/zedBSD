#!/bin/sh
# ws299-p004: QEMU check of the raw block device read-ahead.  Beside the boot disk one blank 1 GiB NVMe (nvme1n1).
# The disk starts with an empty MBR (a valid table without partitions, so that a partition reload succeeds).
# The guest writes the same 200 MiB of random data at 1 MiB and at 300 MiB, drops the cache (diskpart reload: the
# partition reload stops read-ahead on the disk, waits for it and invalidates the disk's lines), then checks with cksum:
#   (a) a cold sequential read (read ahead) and the same read again (from the cache);
#   (b) two readers at once on the same cold region (1 MiB and 64 KiB reads, two streams);
#   (d) after a read ahead past 201 MiB, 1 MiB of new data written at 201 MiB is what a later read sees;
#   (c) ten times a cold read of 64 MiB followed at once by a partition reload, none refused (no false EBUSY from
#       read-ahead still running);
#   (e) a cold 4 KiB-block sequential read.
# and prints the read-ahead log line and the I/O counters.
#   sh plan/ws299/tests/qemu-readahead.sh IMAGE      (IMAGE built with CONFIG_PCAT_SERIAL_MIRROR=y)
# W is ${WS299_RUNS:-build/tmp}/qemu-readahead (a fresh run each time).  The last line is "qemu-readahead: PASS|FAIL ...".
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
set -u
. plan/tools/guest/qemu-accel.sh
. plan/tools/fresh-out.sh
IMAGE=${1:?image}
mkdir -p "${WS299_RUNS:-build/tmp}"; fresh_out "${WS299_RUNS:-build/tmp}"/qemu-readahead
W=${WS299_RUNS:-build/tmp}/qemu-readahead
truncate -s 1G "$W/d.img"
printf '\125\252' | dd of="$W/d.img" bs=1 seek=510 conv=notrunc 2> /dev/null
cp /usr/share/OVMF/OVMF_VARS_4M.fd "$W/vars.fd"
timeout 1800 qemu-system-x86_64 -name p1-readahead -machine q35 -m 8G -smp 4 $(qemu_accel_args max) \
  -drive "if=pflash,format=raw,readonly=on,file=/usr/share/OVMF/OVMF_CODE_4M.fd" \
  -drive "if=pflash,format=raw,file=$W/vars.fd" \
  -drive "if=none,id=boot,file=$IMAGE,format=raw,snapshot=on" \
  -device nvme,serial=zedbsd-boot,drive=boot,bootindex=1 \
  -drive "if=none,id=d,file=$W/d.img,format=raw" \
  -device nvme,serial=zedbsd-ra,drive=d \
  -vga std -display none -no-reboot \
  -serial "unix:$W/serial.sock,server=on,wait=off" \
  -qmp "unix:$W/qmp.sock,server=on,wait=off" \
  -gdb "tcp:127.0.0.1:${GDB_PORT:-22212}" > "$W/qemu.log" 2>&1 &
pid=$!
trap 'kill $pid 2> /dev/null' EXIT
i=0
while [ ! -S "$W/serial.sock" ] && [ $i -lt 600 ]; do sleep 0.1; i=$((i + 1)); done
S="python3 plan/tools/guest/serial.py --socket $W/serial.sock --timeout 900"
$S expect 'login: ' > /dev/null || { echo "qemu-readahead: FAIL no login"; exit 1; }
$S login > /dev/null
out=$W/guest.txt
: > "$out"
D=/dev/nvme1n1
DROP="diskpart reload $D > /dev/null; echo reload=\$?"
for command in \
	"dd if=/dev/urandom of=/root/src.bin bs=1024k count=200 2> /dev/null; cksum < /root/src.bin" \
	"dd if=/root/src.bin of=$D bs=1024k seek=1 conv=notrunc 2> /dev/null; dd if=/root/src.bin of=$D bs=1024k seek=300 conv=notrunc 2> /dev/null; $DROP" \
	"dd if=$D bs=1024k skip=1 count=200 2> /tmp/e | cksum; tail -1 /tmp/e" \
	"dd if=$D bs=1024k skip=1 count=200 2> /tmp/e | cksum; tail -1 /tmp/e" \
	"dd if=$D bs=1024k count=200 skip=300 2> /dev/null | cksum > /tmp/b1 & dd if=$D bs=64k count=3200 skip=4800 2> /dev/null | cksum > /tmp/b2 & wait; cat /tmp/b1 /tmp/b2" \
	"dd if=/dev/urandom of=/root/new.bin bs=1024k count=1 2> /dev/null; cksum < /root/new.bin | sed 's/^/new /'" \
	"dd if=/root/new.bin bs=1024k seek=201 conv=notrunc of=$D 2> /dev/null; dd if=$D bs=1024k skip=201 count=1 2> /dev/null | cksum | sed 's/^/new /'" \
	"i=0; f=0; while [ \$i -lt 10 ]; do dd if=$D of=/dev/null bs=1024k count=64 skip=\$((i * 64 + 1)) 2> /dev/null; diskpart reload $D > /dev/null 2>&1 || f=\$((f + 1)); i=\$((i + 1)); done; echo \"refused \$f\"" \
	"dd if=$D bs=4k count=51200 skip=76800 2> /tmp/e | cksum; tail -1 /tmp/e" \
	"dmesg | grep -E 'buf: readahead' | tail -4; sysctl vfs.io.stats; sysctl vfs.bufcache.stats"; do
	echo "guest\$ $command" | tee -a "$out"
	$S run "$command" | tee -a "$out"
done
tr -d '\r' < "$out" > "$out.clean"
src=$(grep -E '^[0-9]+ 209715200$' "$out.clean" | head -1)
same=$(grep -cxF "$src" "$out.clean")
new=$(grep -E '^new [0-9]+ 1048576$' "$out.clean" | sort -u | wc -l)
newcount=$(grep -cE '^new [0-9]+ 1048576$' "$out.clean")
refused=$(sed -n 's/^refused \([0-9]*\)$/\1/p' "$out.clean")
# src.bin's own checksum, then (a) twice, (b) twice and (e): six lines.
reloads=$(grep -c '^reload=0$' "$out.clean")
if [ "$same" -eq 6 ] && [ "$newcount" -eq 2 ] && [ "$new" -eq 1 ] && [ "${refused:-x}" = 0 ] && [ "$reloads" -eq 1 ]; then
	echo "qemu-readahead: PASS (6 equal checksums, new data seen, 0 reloads refused)"
else
	echo "qemu-readahead: FAIL (checksums $same/6, new $newcount/$new, refused ${refused:-?}, reload ok $reloads/1)"
fi

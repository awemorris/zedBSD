#!/bin/sh
# ws299: a file write on the root UFS with a 32 MiB buffer cache (IMAGE built with config-qemu-cache32.mk), where
# the cache was as large as the old fixed dirty bound.  The guest writes MIB MiB of random data to a file, syncs,
# and checks the file against its source; a write that never ends is cut by the guest command's timeout.
#   sh plan/ws299/tests/qemu-cache32-write.sh IMAGE [VMUNIX] [MIB]
# VMUNIX, when given, replaces the image's kernel in a copy of the image (the FAT boot partition), so one image
# tests another kernel built with the same configuration.  W is ${WS299_RUNS:-build/tmp}/qemu-cache32-write (fresh each run).
# The last line is "qemu-cache32-write: PASS|FAIL ...".
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
set -u
. plan/tools/guest/qemu-accel.sh
. plan/tools/fresh-out.sh
IMAGE=${1:?image}
KERNEL=${2:-}
MIB=${3:-200}
mkdir -p "${WS299_RUNS:-build/tmp}"; fresh_out "${WS299_RUNS:-build/tmp}"/qemu-cache32-write
W=${WS299_RUNS:-build/tmp}/qemu-cache32-write
if [ -n "$KERNEL" ]; then
	cp "$IMAGE" "$W/image.img"
	mcopy -o -i "$W/image.img@@$((133120 * 512))" "$KERNEL" ::/vmunix || { echo "qemu-cache32-write: FAIL kernel copy"; exit 1; }
	IMAGE=$W/image.img
fi
cp /usr/share/OVMF/OVMF_VARS_4M.fd "$W/vars.fd"
timeout 1800 qemu-system-x86_64 -name p1-cache32 -machine q35 -m 4G -smp 4 $(qemu_accel_args max) \
  -drive "if=pflash,format=raw,readonly=on,file=/usr/share/OVMF/OVMF_CODE_4M.fd" \
  -drive "if=pflash,format=raw,file=$W/vars.fd" \
  -drive "if=none,id=boot,file=$IMAGE,format=raw,snapshot=on" \
  -device nvme,serial=zedbsd-boot,drive=boot,bootindex=1 \
  -vga std -display none -no-reboot \
  -serial "unix:$W/serial.sock,server=on,wait=off" \
  -qmp "tcp:127.0.0.1:${QMP_PORT:-22211},server=on,wait=off" \
  -gdb "tcp:127.0.0.1:${GDB_PORT:-22212}" > "$W/qemu.log" 2>&1 &
pid=$!
trap 'kill $pid 2> /dev/null' EXIT
i=0
while [ ! -S "$W/serial.sock" ] && [ $i -lt 600 ]; do sleep 0.1; i=$((i + 1)); done
S="python3 plan/tools/guest/serial.py --socket $W/serial.sock --timeout 300"
$S expect 'login: ' > /dev/null || { echo "qemu-cache32-write: FAIL no login"; exit 1; }
$S login > /dev/null
out=$W/guest.txt
: > "$out"
for command in \
	"sysctl vfs.bufcache.max_bytes; dd if=/dev/urandom of=/root/src.bin bs=1024k count=$MIB 2> /dev/null; cksum < /root/src.bin | sed 's/^/src /'" \
	"dd if=/root/src.bin of=/root/dst.bin bs=1024k 2> /dev/null; sync; echo written" \
	"cksum < /root/dst.bin | sed 's/^/dst /'; sysctl vfs.bufcache.stats"; do
	echo "guest\$ $command" | tee -a "$out"
	$S run "$command" | tee -a "$out"
done
tr -d '\r' < "$out" > "$out.clean"
src=$(sed -n 's/^src //p' "$out.clean" | head -1)
dst=$(sed -n 's/^dst //p' "$out.clean" | head -1)
if grep -qx written "$out.clean" && [ -n "$src" ] && [ "$src" = "$dst" ]; then
	echo "qemu-cache32-write: PASS ($MIB MiB written and synced, checksums equal)"
else
	echo "qemu-cache32-write: FAIL (written=$(grep -cx written "$out.clean") src=$src dst=$dst)"
fi

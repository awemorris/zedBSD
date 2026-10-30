#!/bin/sh
# ws299: QEMU stand-in for the 5330's grown NVMe root and the reboot after a large write.  A 1 GiB UFS volume made like
# the image's root (65536 inodes: 256 groups of 4 MiB) is grown to 15104 MiB with plan/tools/hw5330/ufs-grow.py and
# attached as a second NVMe; the guest mounts it, writes GIB GiB of zeros and a 256 MiB random file, syncs, reads
# 2 GiB of the boot disk raw, checks both files with cksum and reboots with the volume still mounted (as the 5330's root was).  QEMU runs with -no-reboot, so it
# ends when the guest resets; the script prints how long the reboot took, or leaves QEMU running for gdb (port
# GDB_PORT) and QMP (W/qmp.sock) when it has not reset within 300 s.
#   sh plan/ws299/tests/qemu-grow-reboot.sh IMAGE [GIB]          (IMAGE built with CONFIG_PCAT_SERIAL_MIRROR=y)
# W is ${WS299_RUNS:-build/tmp}/qemu-grow-reboot (a fresh run each time).  The last line is "qemu-grow-reboot: PASS|FAIL ...".
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
set -u
. plan/tools/guest/qemu-accel.sh
. plan/tools/fresh-out.sh
IMAGE=${1:?image}
GIB=${2:-3}
GDB_PORT=${GDB_PORT:-12990}
mkdir -p "${WS299_RUNS:-build/tmp}"; fresh_out "${WS299_RUNS:-build/tmp}"/qemu-grow-reboot
W=${WS299_RUNS:-build/tmp}/qemu-grow-reboot
mkdir -p "$W/tree/etc"
[ -x build/zedimage-host ] || make build/zedimage-host > /dev/null
build/zedimage-host ufs 1024M "$W/tree" "$W/vol.ufs" --inodes=65536
python3 -I plan/tools/hw5330/ufs-grow.py --size $((15104 << 20)) "$W/vol.ufs" || { echo "qemu-grow-reboot: FAIL grow"; exit 1; }
cp /usr/share/OVMF/OVMF_VARS_4M.fd "$W/vars.fd"
qemu-system-x86_64 -machine q35 -m 8G -smp 4 $(qemu_accel_args max) \
  -drive "if=pflash,format=raw,readonly=on,file=/usr/share/OVMF/OVMF_CODE_4M.fd" \
  -drive "if=pflash,format=raw,file=$W/vars.fd" \
  -drive "if=none,id=boot,file=$IMAGE,format=raw,snapshot=on" \
  -device nvme,serial=zedbsd-boot,drive=boot,bootindex=1 \
  -drive "if=none,id=vol,file=$W/vol.ufs,format=raw" \
  -device nvme,serial=zedbsd-vol,drive=vol \
  -vga std -display none -no-reboot \
  -serial "unix:$W/serial.sock,server=on,wait=off" \
  -qmp "unix:$W/qmp.sock,server=on,wait=off" \
  -gdb "tcp:127.0.0.1:$GDB_PORT" > "$W/qemu.log" 2>&1 &
pid=$!
i=0
while [ ! -S "$W/serial.sock" ] && [ $i -lt 600 ]; do sleep 0.1; i=$((i + 1)); done
S="python3 plan/tools/guest/serial.py --socket $W/serial.sock --timeout 900"
$S expect 'login: ' > /dev/null || { echo "qemu-grow-reboot: FAIL no login"; kill $pid; exit 1; }
$S login > /dev/null
for command in 'mkdir -p /vol && mount -t ufs /dev/nvme1n1 /vol && df -k /vol' \
	"dd if=/dev/zero of=/vol/zero.bin bs=1024k count=$((GIB * 1024)) && dd if=/dev/urandom of=/vol/rand.bin bs=1024k count=256 && sync && cksum /vol/rand.bin" \
	'dd if=/dev/nvme0n1 of=/dev/null bs=1024k count=2048 && cksum /vol/rand.bin && cksum /vol/zero.bin && df -k /vol' \
	'sysctl vfs.bufcache.stats'; do
	echo "guest\$ $command"
	$S run "$command"
done
start=$(date +%s)
echo "guest\$ reboot"
python3 plan/tools/guest/serial.py --socket "$W/serial.sock" --timeout 5 run reboot > /dev/null 2>&1
while kill -0 $pid 2> /dev/null; do
	if [ $(($(date +%s) - start)) -gt 300 ]; then
		echo "qemu-grow-reboot: FAIL no reset in 300 s; QEMU $pid left for gdb 127.0.0.1:$GDB_PORT and $W/qmp.sock"
		exit 1
	fi
	sleep 1
done
echo "qemu-grow-reboot: PASS reset after $(($(date +%s) - start)) s"

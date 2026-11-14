#!/bin/sh
# ws308-p002: QEMU check of the NVMe paths where many threads sleep on their slots and the commands do not complete:
# the command timeout, and a reboot (the controller's stop) in the middle of depth-32 I/O.  Beside the boot disk a
# 1 GiB NVMe behind QEMU's throttle filter at THROTTLE_IOPS (default 2) operations per second, so that with 32
# direct readers most commands wait in QEMU longer than the driver's 10 s timeout.  The guest runs:
#   (1) 32 threads of direct random 4 KiB reads of the throttled disk for 20 s: the commands time out, the queue is
#       reset and the requests are tried again; randread must come back (with or without a failed read),
#   (2) a read of the boot disk and "echo alive": the guest is still usable,
#   (3) 32 threads of direct reads of the boot disk in the background, and a reboot 3 s later: QEMU (-no-reboot)
#       must end within 300 s.
#   sh plan/tools/kstress/qemu-nvme-timeout.sh IMAGE
# IMAGE: plan/tools/posix/config-amd64-serial.mk with randread in /root (ROOTFS_EXTRA_FILES).  W is
# build/tmp/qemu-nvme-timeout (a fresh run each time).  The last line is "qemu-nvme-timeout: PASS|FAIL ...".
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
set -u
. plan/tools/guest/qemu-accel.sh
. plan/tools/fresh-out.sh
IMAGE=${1:?image}
fresh_out build/tmp/qemu-nvme-timeout
W=build/tmp/qemu-nvme-timeout
truncate -s 1G "$W/d.img"
cp /usr/share/OVMF/OVMF_VARS_4M.fd "$W/vars.fd"
timeout 1800 qemu-system-x86_64 -name p1-nvme-timeout -machine q35 -m 4G -smp 8 $(qemu_accel_args max) \
  -drive "if=pflash,format=raw,readonly=on,file=/usr/share/OVMF/OVMF_CODE_4M.fd" \
  -drive "if=pflash,format=raw,file=$W/vars.fd" \
  -drive "if=none,id=boot,file=$IMAGE,format=raw,snapshot=on" \
  -device nvme,serial=zedbsd-boot,drive=boot,bootindex=1 \
  -object "throttle-group,id=tg0,x-iops-total=${THROTTLE_IOPS:-2}" \
  -blockdev "driver=file,filename=$W/d.img,node-name=f0" \
  -blockdev "driver=raw,file=f0,node-name=r0" \
  -blockdev "driver=throttle,throttle-group=tg0,file=r0,node-name=t0" \
  -device nvme,serial=zedbsd-slow,drive=t0 \
  -vga std -display none -no-reboot \
  -serial "unix:$W/serial.sock,server=on,wait=off" > "$W/qemu.log" 2>&1 &
pid=$!
trap 'kill $pid 2> /dev/null' EXIT
i=0
while [ ! -S "$W/serial.sock" ] && [ $i -lt 600 ]; do sleep 0.1; i=$((i + 1)); done
S="python3 plan/tools/guest/serial.py --socket $W/serial.sock --timeout 900"
$S expect 'login: ' > /dev/null || { echo "qemu-nvme-timeout: FAIL no login"; exit 1; }
$S login > /dev/null
out=$W/guest.txt
: > "$out"
start=$(date +%s)
for command in '/root/randread /dev/nvme1n1 4096 32 20 0 64 direct notime; echo slow-rc=$?' \
	'dd if=/dev/nvme0n1 of=/dev/null bs=1024k count=64 2>&1 | tail -1; echo alive'; do
	echo "guest\$ $command" >> "$out"
	$S run "$command" >> "$out" 2>&1
done
slow_seconds=$(($(date +%s) - start))
tr -d '\r' < "$out" > "$out.clean"
slow=$(grep -c '^slow-rc=[01]$' "$out.clean")
alive=$(grep -c '^alive' "$out.clean")
python3 plan/tools/guest/serial.py --socket "$W/serial.sock" --timeout 10 run \
	'(/root/randread /dev/nvme0n1 4096 32 30 0 512 direct notime > /dev/null 2>&1 &) ; sleep 3; reboot' > /dev/null 2>&1
start=$(date +%s)
reset=0
while [ $(($(date +%s) - start)) -le 300 ]; do
	if ! kill -0 $pid 2> /dev/null; then
		reset=1
		break
	fi
	sleep 1
done
reset_seconds=$(($(date +%s) - start))
if [ "$slow" -eq 1 ] && [ "$alive" -eq 1 ] && [ "$reset" -eq 1 ]; then
	echo "qemu-nvme-timeout: PASS (timeouts came back in $slow_seconds s, $(grep '^slow-rc=' "$out.clean"), guest alive, reset ${reset_seconds} s after the reboot under I/O)"
else
	echo "qemu-nvme-timeout: FAIL (slow run $slow/1 after $slow_seconds s, alive $alive, reset $reset after $reset_seconds s)"
fi

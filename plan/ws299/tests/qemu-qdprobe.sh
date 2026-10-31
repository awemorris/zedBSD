#!/bin/sh
# ws299: QEMU probe of what stops 4 KiB reads at depth 32 (the 5330 tops out near 115k reads a second).  The guest
# (8 CPUs, booted with kern.perf=1 so that the kernel reports the VM metadata lock's takes and waits every 2 s) runs
# randread at depth 32 for SECONDS each: getppid() alone (threads, processes), reads of /dev/zero (no disk: a system
# call, a pin of the buffer and a copy), and direct and cached reads of a 2 GiB NVMe disk (threads, processes), and
# prints the kernel's perf lines of each run.
#   sh plan/ws299/tests/qemu-qdprobe.sh IMAGE [SECONDS]
# IMAGE: plan/tools/posix/config-amd64-serial.mk, ZEDBSD_BOOT_EXTRA_LINES='kern.perf=1', and
# ZEDBSD_EXTRA_FILES='--file /root/randread=OUT/randread-zedbsd' (OUT from build-randread.sh).
# W is ${WS299_RUNS:-build/tmp}/qemu-qdprobe (a fresh run each time).  The output is the guest's lines; the last line is
# "qemu-qdprobe: done N runs" or "qemu-qdprobe: FAIL ...".
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
set -u
. plan/tools/guest/qemu-accel.sh
. plan/tools/fresh-out.sh
IMAGE=${1:?image}
SECONDS_EACH=${2:-10}
mkdir -p "${WS299_RUNS:-build/tmp}"; fresh_out "${WS299_RUNS:-build/tmp}"/qemu-qdprobe
W=${WS299_RUNS:-build/tmp}/qemu-qdprobe
truncate -s 2G "$W/nvme.img"
cp /usr/share/OVMF/OVMF_VARS_4M.fd "$W/vars.fd"
timeout 1800 qemu-system-x86_64 -machine q35 -m 4G -smp 8 $(qemu_accel_args max) \
  -drive "if=pflash,format=raw,readonly=on,file=/usr/share/OVMF/OVMF_CODE_4M.fd" \
  -drive "if=pflash,format=raw,file=$W/vars.fd" \
  -drive "if=none,id=boot,file=$IMAGE,format=raw,snapshot=on" \
  -device nvme,serial=zedbsd-boot,drive=boot,bootindex=1 \
  -drive "if=none,id=d1,file=$W/nvme.img,format=raw,cache=unsafe" \
  -device nvme,serial=zedbsd-probe,drive=d1 \
  -vga std -display none -no-reboot \
  -serial "unix:$W/serial.sock,server=on,wait=off" > "$W/qemu.log" 2>&1 &
pid=$!
trap 'kill $pid 2> /dev/null' EXIT
i=0
while [ ! -S "$W/serial.sock" ] && [ $i -lt 600 ]; do sleep 0.1; i=$((i + 1)); done
S="python3 plan/tools/guest/serial.py --socket $W/serial.sock --timeout 600"
$S expect 'login: ' > /dev/null || { echo "qemu-qdprobe: FAIL no login"; exit 1; }
$S login > /dev/null
out=$W/guest.txt
: > "$out"
$S run 'dd if=/dev/zero of=/dev/nvme1n1 bs=1024k count=2048 2> /dev/null; echo filled' >> "$out"
runs=0
for how in "nullcall notime" "nullcall procs notime" "zero" "zero procs" "direct" "direct procs" "cached" "cached procs"; do
	case "$how" in
	zero*) file=/dev/zero; args=$(echo "$how" | sed 's/zero//') ;;
	cached*) file=/dev/nvme1n1; args=$(echo "$how" | sed 's/cached//') ;;
	*) file=/dev/nvme1n1; args=$how ;;
	esac
	command="/root/randread $file 4096 32 $SECONDS_EACH 0 2048 $args notime; dmesg | grep 'perf at_ms' | tail -$((SECONDS_EACH / 2 - 1)) | sed 's/.*\\(switches=[0-9]*\\).*\\(meta takes=[0-9]* waits=[0-9]* max_us=[0-9]*\\).*\\(hold max_us=[0-9]*\\).*/\\1 \\2 \\3/'"
	echo "run qd32 $how" | tee -a "$out"
	$S run "$command" | tee -a "$out"
	runs=$((runs + 1))
done
echo "qemu-qdprobe: done $runs runs"

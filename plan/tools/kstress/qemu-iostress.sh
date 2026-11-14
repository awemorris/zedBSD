#!/bin/sh
# ws308-p006: QEMU run of iostress.c (reads from many threads while munmap, mprotect, fork, readv, a memory hog and
# seteuid change the process under them) on the boot disk's NVMe and on a second NVMe with random data, direct and
# through the cache.  The guest has MEMORY (default 1G), so "hog" pushes pages to swap.  Each run lasts SECONDS
# (default 30).  Then four processes write 32 MiB each with O_DIRECT 4 KiB requests and fsync, at once, and the guest
# must still answer.
#   [IOSTRESS_RUNS='nvme1n1 direct;nvme0n1 hog 1200'] sh plan/tools/kstress/qemu-iostress.sh IMAGE
# QEMU_EXTRA: more QEMU arguments (for example "-gdb tcp::22212" to look at the guest with gdb while it runs).
# IOSTRESS_RUNS: the runs, ";" between them, each a disk under /dev and iostress's words (default: direct, cached,
# direct with seteuid, cached with a 1200 MiB hog, and the boot disk direct with the hog and seteuid).
# IMAGE: plan/tools/posix/config-amd64-serial.mk with /root/iostress and /root/directio (ROOTFS_EXTRA_FILES, from
# plan/tools/kstress/build.sh and plan/ws299/tests/build-randread.sh).  W is build/tmp/qemu-iostress (a fresh run each time).  The last line is "qemu-iostress: PASS|FAIL ...".
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
set -u
. plan/tools/guest/qemu-accel.sh
. plan/tools/fresh-out.sh
IMAGE=${1:?image}
SECONDS_RUN=${SECONDS_RUN:-30}
fresh_out build/tmp/qemu-iostress
W=build/tmp/qemu-iostress
dd if=/dev/urandom of="$W/d.img" bs=1M count=256 status=none
truncate -s 1G "$W/d.img"
cp /usr/share/OVMF/OVMF_VARS_4M.fd "$W/vars.fd"
timeout 3600 qemu-system-x86_64 -name p1-iostress -machine q35 -m "${MEMORY:-1G}" -smp 8 $(qemu_accel_args max) \
  -drive "if=pflash,format=raw,readonly=on,file=/usr/share/OVMF/OVMF_CODE_4M.fd" \
  -drive "if=pflash,format=raw,file=$W/vars.fd" \
  -drive "if=none,id=boot,file=$IMAGE,format=raw,snapshot=on" \
  -device nvme,serial=zedbsd-boot,drive=boot,bootindex=1 \
  -drive "if=none,id=d1,file=$W/d.img,format=raw" \
  -device nvme,serial=zedbsd-data,drive=d1 \
  -vga std -display none -no-reboot ${QEMU_EXTRA:-} \
  -serial "unix:$W/serial.sock,server=on,wait=off" > "$W/qemu.log" 2>&1 &
pid=$!
trap 'kill $pid 2> /dev/null' EXIT
i=0
while [ ! -S "$W/serial.sock" ] && [ $i -lt 600 ]; do sleep 0.1; i=$((i + 1)); done
S="python3 plan/tools/guest/serial.py --socket $W/serial.sock --timeout 900"
$S expect 'login: ' > /dev/null || { echo "qemu-iostress: FAIL no login"; exit 1; }
$S login > /dev/null
out=$W/guest.txt
: > "$out"
runs=0
RUNS=${IOSTRESS_RUNS:-"nvme1n1 direct;nvme1n1;nvme1n1 direct setuid;nvme1n1 hog 1200;nvme0n1 direct hog 1200 setuid"}
IFS=';'
set -- $RUNS
unset IFS
for how in "$@"; do
	command="/root/iostress /dev/${how%% *} $SECONDS_RUN ${how#* } size 256; echo iostress-rc=\$?"
	[ "${how#* }" = "$how" ] && command="/root/iostress /dev/$how $SECONDS_RUN size 256; echo iostress-rc=\$?"
	echo "guest\$ $command" >> "$out"
	$S run "$command" >> "$out" 2>&1
	runs=$((runs + 1))
done
# Direct 4 KiB writes from four processes at once, each ending with fsync (a flush waits for the writes in flight).
command='for m in 300 400 500 600; do /root/directio write /dev/nvme1n1 32 4 $m & done; sync; wait; echo writes-done'
echo "guest\$ $command" >> "$out"
$S run "$command" >> "$out" 2>&1
echo 'guest$ echo alive' >> "$out"
$S run 'echo alive' >> "$out" 2>&1
tr -d '\r' < "$out" > "$out.clean"
passed=$(grep -c 'result=PASS' "$out.clean")
rc0=$(grep -c '^iostress-rc=0' "$out.clean")
alive=$(grep -c '^alive' "$out.clean")
writes=$(grep -c '^directio write mib=32' "$out.clean")
if [ "$passed" -eq "$runs" ] && [ "$rc0" -eq "$runs" ] && [ "$writes" -eq 4 ] && [ "$alive" -eq 1 ]; then
	echo "qemu-iostress: PASS ($passed of $runs runs, 4 direct writers with fsync, guest alive)"
else
	echo "qemu-iostress: FAIL ($passed of $runs runs passed, rc 0 in $rc0, writers $writes/4, alive $alive)"
fi

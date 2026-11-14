#!/bin/sh
# ws308-p006: QEMU run of sigexec.c (exec of a multi-threaded process while SIGUSR1, SIGSTOP, SIGCONT and SIGKILL
# arrive) on an 8-CPU guest, then the guest must still answer.
#   [ROUNDS=300] sh plan/tools/kstress/qemu-sigexec.sh IMAGE
# IMAGE: plan/tools/posix/config-amd64-serial.mk with ROOTFS_EXTRA_FILES='--file /root/sigexec=OUT/sigexec-zedbsd'
# (OUT from plan/tools/kstress/build.sh).  W is build/tmp/qemu-sigexec (a fresh run each time).  The last line
# is "qemu-sigexec: PASS|FAIL ...".
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
set -u
. plan/tools/guest/qemu-accel.sh
. plan/tools/fresh-out.sh
IMAGE=${1:?image}
fresh_out build/tmp/qemu-sigexec
W=build/tmp/qemu-sigexec
cp /usr/share/OVMF/OVMF_VARS_4M.fd "$W/vars.fd"
timeout 1800 qemu-system-x86_64 -name p1-sigexec -machine q35 -m 1G -smp 8 $(qemu_accel_args max) \
  -drive "if=pflash,format=raw,readonly=on,file=/usr/share/OVMF/OVMF_CODE_4M.fd" \
  -drive "if=pflash,format=raw,file=$W/vars.fd" \
  -drive "if=none,id=boot,file=$IMAGE,format=raw,snapshot=on" \
  -device nvme,serial=zedbsd-boot,drive=boot,bootindex=1 \
  -vga std -display none -no-reboot ${QEMU_EXTRA:-} \
  -serial "unix:$W/serial.sock,server=on,wait=off" > "$W/qemu.log" 2>&1 &
pid=$!
trap 'kill $pid 2> /dev/null' EXIT
i=0
while [ ! -S "$W/serial.sock" ] && [ $i -lt 600 ]; do sleep 0.1; i=$((i + 1)); done
S="python3 plan/tools/guest/serial.py --socket $W/serial.sock --timeout 1200"
$S expect 'login: ' > /dev/null || { echo "qemu-sigexec: FAIL no login"; exit 1; }
$S login > /dev/null
out=$W/guest.txt
command="/root/sigexec ${ROUNDS:-300}; echo sigexec-rc=\$?"
echo "guest\$ $command" > "$out"
$S run "$command" >> "$out" 2>&1
$S run 'echo alive' >> "$out" 2>&1
tr -d '\r' < "$out" > "$out.clean"
grep '^sigexec ' "$out.clean"
if grep -q '^sigexec: PASS' "$out.clean" && grep -q '^sigexec-rc=0' "$out.clean" && grep -q '^alive' "$out.clean"; then
	echo "qemu-sigexec: PASS"
else
	echo "qemu-sigexec: FAIL ($(grep -c '^alive' "$out.clean") alive)"
fi

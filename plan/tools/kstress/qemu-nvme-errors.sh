#!/bin/sh
# ws308-p002: QEMU check of the NVMe completion paths with errors and many waiters.  Beside the boot disk a 1 GiB NVMe
# behind QEMU's blkdebug, which fails every read of sector 1024 (512 KiB in) with EIO.  The guest runs, 3 times each:
# 32 threads of direct random 4 KiB reads over the first MiB (they reach the bad sector soon, and randread stops at
# the first failed read), then 32 threads of direct and of cached random reads over 64 MiB from 1 MiB (no bad
# sector: they must all succeed), and at last a read of the bad sector alone (EIO) and of the next 4 KiB line (data;
# a cached read of the bad sector's own line fails as a whole).
# The guest must stay usable throughout.
#   sh plan/tools/kstress/qemu-nvme-errors.sh IMAGE
# IMAGE: plan/tools/posix/config-amd64-serial.mk with randread in /root (ROOTFS_EXTRA_FILES).  W is
# build/tmp/qemu-nvme-errors (a fresh run each time).  The last line is "qemu-nvme-errors: PASS|FAIL ...".
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
set -u
. plan/tools/guest/qemu-accel.sh
. plan/tools/fresh-out.sh
IMAGE=${1:?image}
fresh_out build/tmp/qemu-nvme-errors
W=build/tmp/qemu-nvme-errors
truncate -s 1G "$W/d.img"
cat > "$W/blkdebug.conf" <<'EOF'
[inject-error]
event = "read_aio"
errno = "5"
sector = "1024"
once = "off"
immediately = "off"
EOF
cp /usr/share/OVMF/OVMF_VARS_4M.fd "$W/vars.fd"
timeout 1800 qemu-system-x86_64 -name p1-nvme-errors -machine q35 -m 4G -smp 8 $(qemu_accel_args max) \
  -drive "if=pflash,format=raw,readonly=on,file=/usr/share/OVMF/OVMF_CODE_4M.fd" \
  -drive "if=pflash,format=raw,file=$W/vars.fd" \
  -drive "if=none,id=boot,file=$IMAGE,format=raw,snapshot=on" \
  -device nvme,serial=zedbsd-boot,drive=boot,bootindex=1 \
  -drive "if=none,id=d1,file=blkdebug:$W/blkdebug.conf:$W/d.img,format=raw" \
  -device nvme,serial=zedbsd-errors,drive=d1 \
  -vga std -display none -no-reboot \
  -serial "unix:$W/serial.sock,server=on,wait=off" > "$W/qemu.log" 2>&1 &
pid=$!
trap 'kill $pid 2> /dev/null' EXIT
i=0
while [ ! -S "$W/serial.sock" ] && [ $i -lt 600 ]; do sleep 0.1; i=$((i + 1)); done
S="python3 plan/tools/guest/serial.py --socket $W/serial.sock --timeout 600"
$S expect 'login: ' > /dev/null || { echo "qemu-nvme-errors: FAIL no login"; exit 1; }
$S login > /dev/null
out=$W/guest.txt
: > "$out"
round=0
while [ "$round" -lt 3 ]; do
	for command in '/root/randread /dev/nvme1n1 4096 32 5 0 1 direct notime; echo bad-range-rc=$?' \
		'/root/randread /dev/nvme1n1 4096 32 5 1 64 direct notime; echo good-direct-rc=$?' \
		'/root/randread /dev/nvme1n1 4096 32 5 1 64 notime; echo good-cached-rc=$?'; do
		echo "guest\$ $command" >> "$out"
		$S run "$command" >> "$out" 2>&1
	done
	round=$((round + 1))
done
for command in 'dd if=/dev/nvme1n1 of=/dev/null bs=512 skip=1024 count=1 2>&1 | tail -1; echo bad-sector-rc=$?' \
	'dd if=/dev/nvme1n1 bs=512 skip=1032 count=1 2> /dev/null | wc -c' 'echo alive'; do
	echo "guest\$ $command" >> "$out"
	$S run "$command" >> "$out" 2>&1
done
tr -d '\r' < "$out" > "$out.clean"
bad=$(grep -c '^bad-range-rc=1' "$out.clean")
good=$(grep -c '^good-\(direct\|cached\)-rc=0' "$out.clean")
alive=$(grep -c '^alive' "$out.clean")
neighbour=$(grep -c '^ *512$' "$out.clean")
if [ "$bad" -eq 3 ] && [ "$good" -eq 6 ] && [ "$alive" -eq 1 ] && [ "$neighbour" -eq 1 ]; then
	echo "qemu-nvme-errors: PASS (3 failing runs stopped, 6 good runs, guest alive)"
else
	echo "qemu-nvme-errors: FAIL (bad runs $bad/3, good runs $good/6, neighbour $neighbour, alive $alive)"
fi

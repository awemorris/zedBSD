#!/bin/sh
# ws299: a native UFS (mkfs -t ufs --profile=native, the batched journal) cut off while a long file write is in
# flight, on the serial console.  Beside the boot disk a blank 2 GiB NVMe.  First boot: format and mount it, write
# KEPT_MIB MiB of random bytes to /c/kept in 1 MiB writes (many allocation runs, across indirect blocks) and fsync it
# (sync FILE), then write BIG_MIB MiB more to /c/big in the background and cut the machine off (QEMU killed) after
# SECONDS.  Second boot: mount (the journal replays), /c/kept's checksum must be the one recorded, a new file written
# and fsynced reads back after an unmount and a mount, and the volume is checked on the host
# (plan/tools/ufs/check-volume.py) after the guest stops.
#   sh plan/ws299/tests/qemu-ufs-crash.sh IMAGE [SECONDS...]       (default 2 6; IMAGE built with
#                                                                   CONFIG_PCAT_SERIAL_MIRROR=y)
# W is ${WS299_RUNS:-build/tmp}/qemu-ufs-crash (a fresh run each time).  The last line is
# "qemu-ufs-crash: PASS|FAIL ...".
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
set -u
. plan/tools/guest/qemu-accel.sh
. plan/tools/fresh-out.sh
IMAGE=${1:?image}
shift
[ $# -gt 0 ] || set -- 2 6
KEPT_MIB=${KEPT_MIB:-72}
BIG_MIB=${BIG_MIB:-600}
mkdir -p "${WS299_RUNS:-build/tmp}"
fresh_out "${WS299_RUNS:-build/tmp}"/qemu-ufs-crash
W=${WS299_RUNS:-build/tmp}/qemu-ufs-crash
status=0
pid=
trap '[ -n "$pid" ] && kill $pid 2> /dev/null' EXIT

# Boots IMAGE with the disk DISK; RUN is this boot's directory.
boot() {
	cp /usr/share/OVMF/OVMF_VARS_4M.fd "$RUN/vars.fd"
	qemu-system-x86_64 -name p1-ufs-crash -machine q35 -m 4G -smp 4 $(qemu_accel_args max) \
	  -drive "if=pflash,format=raw,readonly=on,file=/usr/share/OVMF/OVMF_CODE_4M.fd" \
	  -drive "if=pflash,format=raw,file=$RUN/vars.fd" \
	  -drive "if=none,id=boot,file=$IMAGE,format=raw,snapshot=on" \
	  -device nvme,serial=zedbsd-boot,drive=boot,bootindex=1 \
	  -drive "if=none,id=c,file=$1,format=raw,cache=writeback" \
	  -device nvme,serial=zedbsd-crash,drive=c \
	  -vga std -display none -no-reboot \
	  -serial "unix:$RUN/serial.sock,server=on,wait=off" > "$RUN/qemu.log" 2>&1 &
	pid=$!
	i=0
	while [ ! -S "$RUN/serial.sock" ] && [ $i -lt 600 ]; do sleep 0.1; i=$((i + 1)); done
	S="python3 plan/tools/guest/serial.py --socket $RUN/serial.sock --timeout 600"
	$S expect 'login: ' > /dev/null || return 1
	$S login > /dev/null || return 1
	return 0
}

# Runs one guest command, keeping its output.
run() {
	echo "guest\$ $1" >> "$RUN/guest.txt"
	$S run "$1" >> "$RUN/guest.txt" 2>&1
}

say() {
	echo "$*"
	case "$*" in
	*" fail"*) status=1 ;;
	esac
}

for seconds in "$@"; do
	disk=$W/d-$seconds.img
	truncate -s 2G "$disk"
	RUN=$W/run1-$seconds
	mkdir -p "$RUN"
	if ! boot "$disk"; then
		say "CRASH $seconds boot1 fail"
		kill $pid 2> /dev/null
		continue
	fi
	run "reg=\$(echo no | mkfs -t ufs --profile=native nvme1n1 2>&1 | sed -n 's/.*registration=\([0-9]*\).*/\1/p'); echo \"FORMAT nvme1n1:\$reg\" | mkfs -t ufs --profile=native nvme1n1 > /dev/null 2>&1; mkdir -p /c; mount -t ufs /dev/nvme1n1 /c && echo MOUNTED"
	run "dd if=/dev/urandom of=/root/kept bs=1048576 count=$KEPT_MIB 2> /dev/null; dd if=/root/kept of=/c/kept bs=1048576 2> /dev/null && sync /c/kept && cksum < /c/kept | sed 's/^/KEPT /'"
	kept=$(tr -d '\r' < "$RUN/guest.txt" | sed -n 's/^KEPT //p' | head -1)
	run "(dd if=/dev/urandom of=/c/big bs=1048576 count=$BIG_MIB > /dev/null 2>&1 &); echo STARTED"
	sleep "$seconds"
	kill -9 $pid 2> /dev/null
	wait $pid 2> /dev/null
	pid=
	if [ -z "$kept" ] || ! tr -d '\r' < "$RUN/guest.txt" | grep -q '^MOUNTED'; then
		say "CRASH $seconds write fail (kept=$kept)"
		continue
	fi

	RUN=$W/run2-$seconds
	mkdir -p "$RUN"
	if ! boot "$disk"; then
		say "CRASH $seconds boot2 fail"
		kill $pid 2> /dev/null
		continue
	fi
	run "mkdir -p /c; mount -t ufs /dev/nvme1n1 /c && echo MOUNTED; cksum < /c/kept | sed 's/^/KEPT /'; ls -l /c"
	run "dd if=/dev/urandom of=/root/after bs=1048576 count=8 2> /dev/null; cp /root/after /c/after && sync /c/after && umount /c && mount -t ufs /dev/nvme1n1 /c && cmp /root/after /c/after && umount /c && echo REMOUNTED"
	kill $pid 2> /dev/null
	wait $pid 2> /dev/null
	pid=
	after=$(tr -d '\r' < "$RUN/guest.txt" | sed -n 's/^KEPT //p' | head -1)
	if [ "$after" = "$kept" ]; then
		say "CRASH $seconds kept ok ($kept)"
	else
		say "CRASH $seconds kept fail (before $kept, after $after)"
	fi
	if tr -d '\r' < "$RUN/guest.txt" | grep -q '^REMOUNTED'; then
		say "CRASH $seconds remount ok"
	else
		say "CRASH $seconds remount fail"
	fi
	if python3 -I plan/tools/ufs/check-volume.py "$disk" > "$RUN/check.txt" 2>&1; then
		say "CRASH $seconds check ok"
	else
		say "CRASH $seconds check fail ($(tail -n 1 "$RUN/check.txt"))"
	fi
done
if [ $status = 0 ]; then
	echo "qemu-ufs-crash: PASS $W"
else
	echo "qemu-ufs-crash: FAIL $W"
	exit 1
fi

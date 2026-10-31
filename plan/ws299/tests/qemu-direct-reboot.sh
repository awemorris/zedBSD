#!/bin/sh
# ws299-p010: QEMU repetition of the 5330 p7direct measurement followed by a reboot, to catch a hang after O_DIRECT
# work (the 5330 once stayed frozen after it).  Each round boots IMAGE with a blank 6 GiB NVMe disk, 8 CPUs, the
# gdbstub (GDB_PORT) and QMP (QMP_PORT); the guest writes SIZE_MIB MiB to the disk with O_DIRECT, reads it back in
# 1 MiB and 4 MiB requests, does random 4 KiB direct reads at depth 1 and 32 (one description, then one per thread),
# a cached read of 256 MiB, and reboots.  The round passes when QEMU exits (-no-reboot) within REBOOT_SECONDS of the
# reboot.  A round that does not: the gdbstub's threads, backtraces and registers go to W/hang-N.txt (symbols from
# VMUNIX), QMP "info registers" for every CPU to W/hang-N-qmp.txt, and the run stops with QEMU left running for a
# debugger (its pid in W/qemu.pid).
#   sh plan/ws299/tests/qemu-direct-reboot.sh IMAGE VMUNIX [ROUNDS]
# IMAGE: plan/tools/posix/config-amd64-serial.mk with ZEDBSD_EXTRA_FILES putting directio and randread in /root.
# W is ${WS299_RUNS:-build/tmp}/qemu-direct-reboot.  The last line is "qemu-direct-reboot: PASS N rounds|HANG round N|FAIL ...".
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
set -u
. plan/tools/guest/qemu-accel.sh
. plan/tools/fresh-out.sh
IMAGE=${1:?image}
VMUNIX=${2:?vmunix}
ROUNDS=${3:-3}
SIZE_MIB=${SIZE_MIB:-4096}
GDB_PORT=${GDB_PORT:-22212}
QMP_PORT=${QMP_PORT:-22211}
REBOOT_SECONDS=${REBOOT_SECONDS:-180}
mkdir -p "${WS299_RUNS:-build/tmp}"; fresh_out "${WS299_RUNS:-build/tmp}"/qemu-direct-reboot
W=${WS299_RUNS:-build/tmp}/qemu-direct-reboot
truncate -s 6G "$W/nvme.img"
cp /usr/share/OVMF/OVMF_VARS_4M.fd "$W/vars.fd"
round=1
while [ "$round" -le "$ROUNDS" ]; do
	rm_sock=$W/serial-$round.sock
	qemu-system-x86_64 -name p1-direct-reboot -machine q35 -m 4G -smp 8 $(qemu_accel_args max) \
	  -drive "if=pflash,format=raw,readonly=on,file=/usr/share/OVMF/OVMF_CODE_4M.fd" \
	  -drive "if=pflash,format=raw,file=$W/vars.fd" \
	  -drive "if=none,id=boot,file=$IMAGE,format=raw,snapshot=on" \
	  -device nvme,serial=zedbsd-boot,drive=boot,bootindex=1 \
	  -drive "if=none,id=d1,file=$W/nvme.img,format=raw,cache=unsafe" \
	  -device nvme,serial=zedbsd-direct,drive=d1 \
	  -vga std -display none -no-reboot \
	  -gdb "tcp:127.0.0.1:$GDB_PORT" \
	  -qmp "tcp:127.0.0.1:$QMP_PORT,server=on,wait=off" \
	  -serial "unix:$rm_sock,server=on,wait=off" > "$W/qemu-$round.log" 2>&1 &
	pid=$!
	echo "$pid" > "$W/qemu.pid"
	i=0
	while [ ! -S "$rm_sock" ] && [ $i -lt 600 ]; do sleep 0.1; i=$((i + 1)); done
	S="python3 plan/tools/guest/serial.py --socket $rm_sock --timeout 900"
	if ! $S expect 'login: ' > /dev/null; then
		echo "qemu-direct-reboot: FAIL no login in round $round"
		kill "$pid"
		exit 1
	fi
	$S login > /dev/null
	out=$W/guest-$round.txt
	: > "$out"
	for command in "/root/directio write /dev/nvme1n1 $SIZE_MIB 1024" \
		"/root/directio read /dev/nvme1n1 $SIZE_MIB 1024; /root/directio read /dev/nvme1n1 $SIZE_MIB 4096" \
		"/root/randread /dev/nvme1n1 4096 1 10 0 $SIZE_MIB direct; /root/randread /dev/nvme1n1 4096 1 10 0 $SIZE_MIB direct fdeach" \
		"/root/randread /dev/nvme1n1 4096 32 10 0 $SIZE_MIB direct; /root/randread /dev/nvme1n1 4096 32 10 0 $SIZE_MIB direct fdeach" \
		"dd if=/dev/nvme1n1 bs=1024k count=256 2> /dev/null | cksum"; do
		echo "guest\$ $command" >> "$out"
		$S run "$command" >> "$out" 2>&1
	done
	echo "guest\$ reboot" >> "$out"
	python3 plan/tools/guest/serial.py --socket $rm_sock --timeout 5 run 'reboot' > /dev/null 2>&1 || true
	i=0
	while kill -0 "$pid" 2> /dev/null && [ $i -lt "$REBOOT_SECONDS" ]; do sleep 1; i=$((i + 1)); done
	if kill -0 "$pid" 2> /dev/null; then
		timeout 120 gdb -batch -nx -ex "file $VMUNIX" -ex "target remote 127.0.0.1:$GDB_PORT" \
		    -ex 'info threads' -ex 'thread apply all bt 30' -ex 'thread apply all info registers rip rsp rflags cr3' \
		    > "$W/hang-$round.txt" 2>&1
		python3 - "$QMP_PORT" > "$W/hang-$round-qmp.txt" 2>&1 <<'PY'
import json, socket, sys
s = socket.create_connection(("127.0.0.1", int(sys.argv[1])), 10)
f = s.makefile("rw")
f.readline()
def cmd(c, **a):
    f.write(json.dumps({"execute": c, "arguments": a} if a else {"execute": c}) + "\n"); f.flush()
    while True:
        r = json.loads(f.readline())
        if "return" in r or "error" in r:
            return r
cmd("qmp_capabilities")
print(cmd("human-monitor-command", **{"command-line": "info cpus"}).get("return", ""))
for n in range(8):
    cmd("human-monitor-command", **{"command-line": "cpu %d" % n})
    print("== cpu", n)
    print(cmd("human-monitor-command", **{"command-line": "info registers"}).get("return", ""))
PY
		echo "qemu-direct-reboot: HANG round $round (QEMU pid $pid left running; $W/hang-$round.txt)"
		exit 2
	fi
	echo "round $round: rebooted after $i s"
	round=$((round + 1))
done
echo "qemu-direct-reboot: PASS $ROUNDS rounds"

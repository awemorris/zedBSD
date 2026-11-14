#!/bin/sh
# The QEMU check of the scheduler's hint interface on x86_64: an image of the base userland with the console on the
# serial port and the kernel with the balancer and its stress test (config-x86_64-balancer-stress.mk beside this
# file, with the objects of desktop-balancer-overlay.mk),
# whose boot line gives a false table of CPU kinds and starts the stress thread (desktop_balancer.classes=PPEE
# desktop_balancer.stress=1).  The guest (4 CPUs, KVM) runs for SECONDS (default 1800) the hint stress programs
# (hintstress.c: threads made and reaped with forked children, timed sleeps checked for waking early, threads that
# compute without a system call) while the host keeps running the kernel stress programs of plan/tools/kstress
# (iostress on a second disk, credrace, sigexec) one after another.  The guest is driven on its serial console
# (plan/tools/guest/serial.py); what the stress thread did is read from the running kernel through QEMU's gdbstub
# (the report sched_hint_test_report), at the start and at the end.
#
#   plan/ws302/tests/p003-qemu.sh OUT [SECONDS]
#
# The image is made in build/x86_64 (its sysroot must be made).  QEMU is named p6-ws302 and its gdbstub listens on
# GDB_PORT (default 22306).  The last line is "p003-qemu: PASS" when the guest answered at the end, every stress
# program passed every run, no timed sleep ended early, and the report shows the stress thread kept running and the
# scheduler both moved waiting threads and handed off running ones.
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
set -u
cd "$(dirname -- "$0")/../../.."
[ $# -ge 1 ] || { echo "usage: p003-qemu.sh OUT [SECONDS]" >&2; exit 2; }
. plan/tools/guest/qemu-accel.sh
out=$(mkdir -p "$1" && cd "$1" && pwd)
seconds=${2:-1800}
gdb_port=${GDB_PORT:-22306}
summary=$out/summary.txt
: > "$summary"
status=PASS
sysroot=build/x86_64/sysroot
clang=build/llvm/bin/clang

# Builds the guest programs: the hint stress programs and the kernel stress programs, static.
for name in plan/ws302/tests/hintstress plan/tools/kstress/iostress plan/tools/kstress/credrace \
	plan/tools/kstress/sigexec; do
	base=$(basename "$name")
	"$clang" --target=x86_64-unknown-zedbsd --sysroot="$sysroot" -nostdinc -isystem include \
		-isystem "$sysroot/usr/include" -O2 -Wall -Wextra -Werror -c "$name.c" -o "$out/$base.o" &&
	"$clang" --target=x86_64-unknown-zedbsd --sysroot="$sysroot" -nostdlib -static "$sysroot/usr/lib/crt0.o" \
		"$out/$base.o" "$sysroot/usr/lib/libc.a" "$sysroot/usr/lib/libzedbsd-compiler-rt.a" \
		-o "$out/$base" || { echo "program $base: does not build" >> "$summary"; status=FAIL; }
done
[ $status = PASS ] || { cat "$summary"; echo "p003-qemu: FAIL"; exit 1; }

# The boot configuration: the platform's, with the false table and the stress thread.
cp mk/platform-x86_64/bootcfg/zedbsd-uefi.cfg "$out/zedbsd.cfg"
printf '%s\n' desktop_balancer.classes=PPEE desktop_balancer.stress=1 >> "$out/zedbsd.cfg"

# Makes the image.
extra="--file /root/hintstress=$out/hintstress --file /root/iostress=$out/iostress"
extra="$extra --file /root/credrace=$out/credrace --file /root/sigexec=$out/sigexec"
timeout 5400 make -j4 -f Makefile -f plan/ws302/tests/desktop-balancer-overlay.mk \
	CONFIGMK=plan/ws302/tests/config-x86_64-balancer-stress.mk ZEDBSD_CONFIG_SRC="$out/zedbsd.cfg" \
	ROOTFS_EXTRA_FILES="$extra" disk \
	> "$out/image.log" 2>&1 < /dev/null
rc=$?
echo "image rc=$rc warnings=$(grep -c 'warning:' "$out/image.log")" >> "$summary"
[ $rc -eq 0 ] || { cat "$summary"; echo "p003-qemu: FAIL"; exit 1; }
cp build/x86_64/hdd-image.img "$out/hdd.img"
cp build/x86_64/vmunix "$out/vmunix"
sync
address=$(build/llvm/bin/llvm-nm "$out/vmunix" | awk '$3 == "sched_hint_test_report" { print $1 }')
[ -n "$address" ] || { echo "report symbol: missing" >> "$summary"; cat "$summary"; echo "p003-qemu: FAIL"; exit 1; }

# Starts the guest: 4 CPUs, a second disk for iostress, the console on a socket, the gdbstub on its port.
dd if=/dev/urandom of="$out/data.img" bs=1M count=256 status=none
truncate -s 1G "$out/data.img"
cp /usr/share/OVMF/OVMF_VARS_4M.fd "$out/vars.fd"
timeout $((seconds + 1800)) qemu-system-x86_64 -name p6-ws302 -machine q35 -m 2G -smp 4 $(qemu_accel_args max) \
	-drive "if=pflash,format=raw,readonly=on,file=/usr/share/OVMF/OVMF_CODE_4M.fd" \
	-drive "if=pflash,format=raw,file=$out/vars.fd" \
	-drive "if=none,id=boot,file=$out/hdd.img,format=raw,snapshot=on" \
	-device nvme,serial=zedbsd-boot,drive=boot,bootindex=1 \
	-drive "if=none,id=d1,file=$out/data.img,format=raw" \
	-device nvme,serial=zedbsd-data,drive=d1 \
	-vga std -display none -no-reboot -gdb "tcp:127.0.0.1:$gdb_port" \
	-serial "unix:$out/serial.sock,server=on,wait=off" > "$out/qemu.log" 2>&1 &
pid=$!
trap 'kill $pid 2> /dev/null' EXIT
i=0
while [ ! -S "$out/serial.sock" ] && [ $i -lt 600 ]; do sleep 0.1; i=$((i + 1)); done
S="python3 -I plan/tools/guest/serial.py --socket $out/serial.sock --timeout 900"
$S expect 'login: ' > /dev/null || { echo "login: none" >> "$summary"; cat "$summary"; echo "p003-qemu: FAIL"; exit 1; }
$S login > /dev/null

# Reads the stress thread's report from the running kernel: the 26 words of the memory dump's lines (gdb also
# prints where it stopped the guest, a line without the colon after the address).
read_report() {
	timeout 120 gdb -batch -nx -ex "target remote 127.0.0.1:$gdb_port" -ex "x/26gx 0x$address" -ex detach \
		< /dev/null 2> /dev/null | grep '^0x[0-9a-f]*:' | sed 's/^[^:]*:[[:space:]]*//' | tr -s ' \t' '\n' |
		grep '^0x'
}

# Starts the hint stress programs in the background for the whole run.
guest=$out/guest.txt
: > "$guest"
$S run "/root/hintstress busy $seconds 3 > /root/busy.txt 2>&1 &" >> "$guest" 2>&1
$S run "/root/hintstress threads $seconds > /root/threads.txt 2>&1 &" >> "$guest" 2>&1
$S run "/root/hintstress sleep $seconds > /root/sleep.txt 2>&1 &" >> "$guest" 2>&1
read_report > "$out/report-start.txt"

# Runs the kernel stress programs one after another until the time is up.
start=$(date +%s)
runs=0
while [ $(( $(date +%s) - start )) -lt "$seconds" ]; do
	$S run "/root/iostress /dev/nvme1n1 60 direct size 256; echo iostress-rc=\$?" >> "$guest" 2>&1
	$S run "/root/credrace 2000; echo credrace-rc=\$?" >> "$guest" 2>&1
	$S run "/root/sigexec 300; echo sigexec-rc=\$?" >> "$guest" 2>&1
	runs=$((runs + 1))
done

# Waits for the hint stress programs, gathers their lines and the report, and asks the guest to answer.
sleep 30
$S run 'wait; cat /root/busy.txt /root/threads.txt /root/sleep.txt' >> "$guest" 2>&1
read_report > "$out/report-end.txt"
$S run 'echo alive' >> "$guest" 2>&1
kill $pid 2> /dev/null
wait $pid 2> /dev/null
sync

# Judges the guest's lines.
tr -d '\r' < "$guest" > "$guest.clean"
iostress_ok=$(grep -c '^iostress-rc=0' "$guest.clean")
credrace_ok=$(grep -c '^credrace-rc=0' "$guest.clean")
sigexec_ok=$(grep -c '^sigexec-rc=0' "$guest.clean")
echo "kstress rounds=$runs iostress-ok=$iostress_ok credrace-ok=$credrace_ok sigexec-ok=$sigexec_ok" >> "$summary"
[ "$runs" -gt 0 ] && [ "$iostress_ok" -eq "$runs" ] && [ "$credrace_ok" -eq "$runs" ] &&
	[ "$sigexec_ok" -eq "$runs" ] || status=FAIL
for mode in busy threads sleep; do
	line=$(grep "^hintstress $mode " "$guest.clean" | tail -1)
	echo "${line:-hintstress $mode: no result}" >> "$summary"
	case $line in
	*result=PASS) ;;
	*) status=FAIL ;;
	esac
done
alive=$(grep -c '^alive' "$guest.clean")
echo "guest alive=$alive" >> "$summary"
[ "$alive" -ge 1 ] || status=FAIL

# Judges the report: the order of struct sched_hint_test_report.
word() {
	sed -n "$(($2 + 1))p" "$1"
}
magic=$(word "$out/report-end.txt" 0)
passes_start=$(printf '%d' "$(word "$out/report-start.txt" 1)" 2> /dev/null || echo 0)
passes_end=$(printf '%d' "$(word "$out/report-end.txt" 1)" 2> /dev/null || echo 0)
names="hints moves refused handoffs handoff_retries handoff_failures starved kernel_held"
line="report magic=$magic passes=$passes_start..$passes_end threads=$(printf '%d' "$(word "$out/report-end.txt" 2)")"
line="$line set[ok,ealready,eagain,ebusy,esrch,einval,other]="
n=4
while [ $n -lt 11 ]; do line="$line$(printf '%d' "$(word "$out/report-end.txt" $n)"),"; n=$((n + 1)); done
line="$line place="
while [ $n -lt 18 ]; do line="$line$(printf '%d' "$(word "$out/report-end.txt" $n)"),"; n=$((n + 1)); done
echo "$line" >> "$summary"
line="counters"
for name in $names; do
	value=$(printf '%d' "$(word "$out/report-end.txt" $n)" 2> /dev/null || echo 0)
	eval "$name=$value"
	line="$line $name=$value"
	n=$((n + 1))
done
echo "$line" >> "$summary"
[ "$magic" = 0x48494e5453545253 ] || status=FAIL
[ "$passes_end" -gt "$passes_start" ] || status=FAIL
[ "${moves:-0}" -gt 0 ] && [ "${handoffs:-0}" -gt 0 ] || status=FAIL

# Reports.
cat "$summary"
echo "p003-qemu: $status"

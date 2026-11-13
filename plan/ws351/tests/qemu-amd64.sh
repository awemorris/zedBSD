#!/bin/sh
# ws351: the variable tick under QEMU+KVM (amd64), end to end; the summary is OUT/summary.txt, its last line
# "qemu-amd64: done" or "qemu-amd64: FAIL ...".
#   sh plan/ws351/tests/qemu-amd64.sh OUT CPU [PORT]
# OUT is a new directory.  CPU is the QEMU -cpu: host,+invtsc times the tick by TSC deadlines, host (the invariant TSC hidden) by the
# periodic timer.  PORT is the host's SSH port (default 2351).
# The image is this tree's build/x86_64 (make CONFIGMK=config/config.x86_64.mk ... disk, the kernel with
# CONFIG_KERN_THERMAL=y CONFIG_DRIVER_X86_POWER_CONTROL=y) with its root tree copied, plus the guest harness's
# network (plan/tools/guest/net.conf) and /bin/clocktool (plan/ws351/tests/clocktool, static-tool.sh), made
# into a disk the way the build does.  The console is not read: the guest is driven over SSH as user/user, and
# the kernel's state (counter_mode, the per-CPU tick_divisor of lapic.c, tick_lowered) through the QEMU monitor.
#  1. clocktool res: clock_getres() and the step two clock_gettime() calls see.
#  2. clocktool sleep 200 1000 on an idle guest: 1 ms sleeps.
#  3. idle: the tick divisor of CPUs 0-3 read 20 times over 10 s (10 = 100 Hz).
#  4. busy: four clocktool spin, the divisors read the same way (1 = 1000 Hz), and sleep 200 1000 again.
# A kernel built with near-thread.patch (test only) also reports which sleepers kept each CPU fast.
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
set -u
OUT=${1:?OUT}
CPU=${2:?CPU}
PORT=${3:-2351}
cd "$(dirname "$0")/../../.."
mkdir -p "$OUT"
SUMMARY=$OUT/summary.txt
: > "$SUMMARY"
say() {
	echo "qemu-amd64: $*" | tee -a "$SUMMARY"
}
fail() {
	say "FAIL $*"
	exit 1
}
B=build/x86_64
for f in $B/vmunix $B/vmunix.map $B/rootfs/bin/sh $B/zedbsd.cfg $B/uefi/BOOTX64.EFI $B/native-swap-1024m.img; do
	[ -e "$f" ] || fail "no $f"
done

# The image: the root tree, the network and the tool.
sh plan/ws333/tests/hw5330/static-tool.sh plan/ws351/tests/clocktool "$OUT/static" > "$OUT/static.log" 2>&1 ||
	fail "clocktool build"
[ ! -e "$OUT/rootfs" ] || fail "$OUT/rootfs is there from an earlier run: give a new OUT"
mkdir -p "$OUT/rootfs"
cp -a "$B/rootfs/." "$OUT/rootfs/"
cp plan/tools/guest/net.conf "$OUT/rootfs/etc/net.conf"
cp "$OUT/static/clocktool" "$OUT/rootfs/bin/clocktool"
build/NoctLang/build/noct --path=mk/tools:mk/old-tools/build mk/tools/make-rootfs.noct --backend "$B/zedimage-host" \
	--force --profile amd64 --output "$OUT/root.ufs" --tree "$OUT/rootfs" --size-mib 1024 --min-inodes 65536 \
	> "$OUT/image.log" 2>&1 || fail "root file system"
python3 mk/platform-x86_64/tools/make-x86_64-hdd-image.py --force --variant uefi --kernel $B/vmunix \
	--zedbsd-config $B/zedbsd.cfg --ufs-root "$OUT/root.ufs" --swap-image $B/native-swap-1024m.img \
	--bootx64 $B/uefi/BOOTX64.EFI --fat-size-mib 256 "$OUT/disk.img" >> "$OUT/image.log" 2>&1 || fail "disk image"
sync
say "image $(sha256sum $B/vmunix | cut -c1-16) cpu=$CPU"

symbol() {
	awk -v name="$1" '$NF == name { print $1; exit }' $B/vmunix.map
}
DIVISOR=$(symbol tick_divisor)
MODE=$(symbol counter_mode)
LOWERED=$(symbol tick_lowered)
GLOBAL=$(symbol global_next)
TICKS=$(symbol kernel_ticks)
[ -n "$DIVISOR" ] && [ -n "$MODE" ] && [ -n "$LOWERED" ] || fail "symbols"

cp /usr/share/OVMF/OVMF_VARS_4M.fd "$OUT/vars.fd"
timeout 600 qemu-system-x86_64 -machine q35 -m 8G -smp 4 -accel kvm -cpu "$CPU" \
	-drive if=pflash,format=raw,readonly=on,file=/usr/share/OVMF/OVMF_CODE_4M.fd \
	-drive if=pflash,format=raw,file="$OUT/vars.fd" \
	-device qemu-xhci,id=xhci,p2=8,p3=8 \
	-drive if=none,id=boot,file="$OUT/disk.img",format=raw \
	-device nvme,serial=zedbsd-boot,drive=boot,bootindex=1 \
	-netdev "user,id=net0,net=10.0.2.0/24,host=10.0.2.2,dhcpstart=10.0.2.15,hostfwd=tcp:127.0.0.1:$PORT-:22" \
	-device usb-net,bus=xhci.0,port=2,id=ecm,netdev=net0,mac=52:54:00:33:51:01,msos-desc=on \
	-vga std -display none -no-reboot \
	-monitor unix:"$OUT/m.sock",server,nowait > "$OUT/qemu.out" 2>&1 &
QEMU=$!
cleanup() {
	kill "$QEMU" 2> /dev/null
	wait "$QEMU" 2> /dev/null
	sync
}
trap cleanup EXIT
hmp() {
	printf '%b' "$1" | timeout 5 socat -t 1 - UNIX-CONNECT:"$OUT/m.sock" 2> /dev/null
}
# The words at an address, as decimal numbers on one line.
words() {
	hmp "x /$2wx 0x$1\n" | tr -d '\r' | grep -a -E '^(0x)?[0-9a-f]+: ' | sed 's/^[^:]*://' | tr -s ' \t' '\n' |
		grep -a '^0x' | head -n "$2" | while read -r w; do printf '%d ' "$w"; done
}
zb() {
	timeout "${TO:-60}" sshpass -p user ssh -n -o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null \
		-o ConnectTimeout=5 -o LogLevel=ERROR -p "$PORT" user@127.0.0.1 "$1"
}
start=$(date +%s)
until TO=10 zb true 2> /dev/null; do
	if [ $(($(date +%s) - start)) -ge 300 ]; then
		hmp "screendump $OUT/no-ssh.ppm\n" > /dev/null
		fail "no SSH in 300 s (screen in $OUT/no-ssh.ppm)"
	fi
	sleep 5
done
say "ssh up after $(($(date +%s) - start)) s"
say "counter_mode=$(words "$MODE" 1)"
say "lvtt $(hmp 'cpu 0\ninfo lapic\ncpu 1\ninfo lapic\ncpu 2\ninfo lapic\ncpu 3\ninfo lapic\ncpu 0\n' |
	grep -a -P '^LVTT\t' | awk '{ print $5 }' | sort | uniq -c | tr -s ' \n' ' ')"
sleep 20

# 1-2: the clock's step and 1 ms sleeps.
zb "/bin/clocktool res" | tee -a "$SUMMARY"
zb "/bin/clocktool sleep 200 1000" | tee -a "$SUMMARY"

# 3: idle.
sample() {
	n=0
	: > "$OUT/$1.txt"
	while [ $n -lt 20 ]; do
		echo "divisor $(words "$DIVISOR" 4) lowered $(words "$LOWERED" 4)" >> "$OUT/$1.txt"
		echo "global-next $(words "$GLOBAL" 2) kernel-ticks $(words "$TICKS" 2)" >> "$OUT/$1-global.txt"
		sleep 0.5
		n=$((n + 1))
	done
	say "$1: $(sort "$OUT/$1.txt" | uniq -c | sort -rn | head -3 | tr -s ' ' | tr '\n' ';')"
	say "$1 CPU 0's timers (32-bit halves): $(head -2 "$OUT/$1-global.txt" | tr '\n' ';')"
}
sample idle

# 4: busy on every CPU.
TO=40 zb "for i in 1 2 3 4; do /bin/clocktool spin 25 > /dev/null & done; sleep 1; /bin/clocktool sleep 200 1000; wait" \
	> "$OUT/busy-sleep.txt" 2>&1 &
BUSY=$!
sleep 3
sample busy
wait $BUSY
say "busy $(cat "$OUT/busy-sleep.txt")"
sample idle-again

# With near-thread.patch: per CPU, how often the idle loop stayed fast for a sleeper, and the last one (pid<<32|tid).
NEAR=$(symbol sched_diag_near_count)
LAST=$(symbol sched_diag_near_last)
if [ -n "$NEAR" ] && [ -n "$LAST" ]; then
	say "near counts (32-bit halves) $(words "$NEAR" 8)"
	say "near last (32-bit halves: tid pid) $(words "$LAST" 8)"
	zb "ps -ax 2> /dev/null || ps" > "$OUT/ps.txt" 2>&1
fi
say "done"

#!/bin/sh
# The display-event fence (GPU_DISPLAY_FENCE, hotplug) on the QEMU Venus guest: builds the guest program and the
# image (plan/ws353/tests/config-amd64-fence.mk, the program inside), boots it with two Venus heads on QEMU's D-Bus
# display, runs the steps from the serial line (plan/tools/guest/serial.py) while plan/tools/guest/venus-head.sh plugs
# and unplugs head 1, reads each step's result file from the guest, and writes one summary.  The console's text is
# never read for the verdict: only the program's result files, fetched with commands on the serial line.
#
#   sh plan/ws353/tests/qemu-display-fence.sh [OUT]
#
# OUT (default build/ws353-p002-qemu) gets summary.txt, the steps' result lines and the timings.  The guest's runtime
# is build/ws353-p002-run (P1's own).  The image is build/x86_64/hdd-image.img: the build keeps the x86_64 sysroot in
# build/x86_64 whatever BUILD says, so the image is built there, one target after the other (sysroot, userland, disk).
# Steps: idle 30 s with no change; wait (head plugged, then unplugged); latency (the wake within 1 s of the change);
# cancel; limit; race (registrations while the head is plugged and unplugged back to back); quiet (the topology
# sequence does not move from one step to the next, where a program opens the GPU and its control queue interrupts,
# but the head does not change).
# The program opens the GPU node first, and the open puts a kernel record on the console the serial line carries; a
# record that lands in a command's status line costs serial.py time.  So each step that changes the head is gated:
# the program makes /tmp/NAME.ready after the open and registers only once the host makes /tmp/NAME.go, so the
# record is out before the registration and the host sees the marker soon after it is made.
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
set -eu
cd "$(dirname -- "$0")/../../.."
out=${1:-build/ws353-p002-qemu}
config=plan/ws353/tests/config-amd64-fence.mk
sysroot=$PWD/build/x86_64/sysroot
program_dir=build/ws353-p002-prog
image=build/x86_64/hdd-image.img
export GUEST_RUNTIME=$PWD/build/ws353-p002-run
. plan/tools/guest/venus-hostmem.sh
. plan/tools/fresh-out.sh
fresh_out "$out"
summary=$out/summary.txt
: > "$summary"
guest="python3 plan/tools/guest/guest.py"

say() {
	echo "$*" | tee -a "$summary"
}

# The sysroot, then the program against it, then the image with the program in it.
timeout 3000 make -j4 CONFIGMK=$config sysroot > "$out/make-sysroot.txt" 2>&1
mkdir -p "$program_dir"
clang=build/llvm/bin/clang
resource=$($clang -print-resource-dir)
$clang --target=x86_64-unknown-zedbsd --sysroot="$sysroot" -m64 -O1 -g -Wall -Wextra -Werror \
    -Wdeclaration-after-statement -DKERN_USER_ABI_LP64 -nostdinc -isystem "$sysroot/usr/include" \
    -isystem "$resource/include" -ffreestanding -fno-pic -fno-pie \
    -c plan/ws353/tests/display-fence-test.c -o "$program_dir/display-fence-test.o"
$clang --target=x86_64-unknown-zedbsd --sysroot="$sysroot" -m64 -nostdlib -static -Wl,--build-id=none \
    -Wl,-T,"$sysroot/usr/lib/zedbsd/x86_64/user.ld" "$sysroot/usr/lib/crt0.o" "$program_dir/display-fence-test.o" \
    -Wl,--start-group "$sysroot/usr/lib/libc.a" "$sysroot/usr/lib/libzedbsd-compiler-rt.a" \
    "$sysroot/usr/lib/libclang_rt.builtins.a" -Wl,--end-group -o "$program_dir/display-fence-test.new"
if ! cmp -s "$program_dir/display-fence-test.new" "$program_dir/display-fence-test"; then
	mv "$program_dir/display-fence-test.new" "$program_dir/display-fence-test"
fi
timeout 5400 make -j4 CONFIGMK=$config userland > "$out/make-userland.txt" 2>&1
timeout 3000 make -j4 CONFIGMK=$config disk > "$out/make-disk.txt" 2>&1
sync

# The guest: Venus with two heads on a private D-Bus display, the renderer the host has.
mkdir -p "$GUEST_RUNTIME"
bus=$(timeout 10 dbus-daemon --session --fork --print-address=1 --print-pid=1 --nopidfile)
printf '%s\n' "$bus" | sed -n 1p > "$GUEST_RUNTIME/dbus.address"
printf '%s\n' "$bus" | sed -n 2p > "$GUEST_RUNTIME/dbus.pid"
DBUS_SESSION_BUS_ADDRESS=$(cat "$GUEST_RUNTIME/dbus.address")
export DBUS_SESSION_BUS_ADDRESS
finish() {
	timeout 60 $guest stop >/dev/null 2>&1 || true
	kill "$(cat "$GUEST_RUNTIME/dbus.pid")" 2>/dev/null || true
	sync
}
trap finish EXIT
# The copy of the 2.4 GB image takes minutes on a busy disk.
timeout 900 $guest start "$image" --cpus 4 --qemu-extra \
    "-object memory-backend-memfd,id=mem,size=8G,share=on -machine memory-backend=mem -device virtio-gpu-gl-pci,id=venus,venus=on,blob=on,hostmem=$VENUS_HOSTMEM,max_outputs=2 -display dbus,gl=on,rendernode=/dev/dri/renderD128" \
    > "$out/start.txt"

# One command on the serial line (it logs in first when needed); its output and exit status.
serial() {
	timeout 150 python3 plan/tools/guest/serial.py --socket "$GUEST_RUNTIME/serial.sock" --timeout 120 run "$1"
}
tries=0
until serial 'uname -a' > "$out/uname.txt" 2>&1; do
	tries=$((tries + 1))
	if [ "$tries" -ge 6 ]; then
		say "qemu-display-fence: FAIL no shell on the serial line (see $out/uname.txt)"
		exit 1
	fi
	sleep 20
done
head1() {
	timeout 30 sh plan/tools/guest/venus-head.sh 1 "$1" >> "$out/heads.txt" 2>&1
}
now_ms() {
	date +%s%3N
}
# QEMU ignores a size equal to the last one, and an unplug of a head never given a size: give head 1 one first,
# then let that change settle before the steps.
head1 1024x768
sleep 3
head1 off
sleep 3
failed=0

# A step started in the background on the guest; its result goes to /tmp/NAME.out there.
step_start() {
	name=$1
	shift
	# The kernel's lines on the same line can hide the command's status: the result file decides.
	serial "rm -f /tmp/$name.out /tmp/$name.armed /tmp/$name.ready /tmp/$name.go; /root/display-fence-test $* > /tmp/$name.out 2>&1 < /dev/null &" \
	    > "$out/$name.start.txt" 2>&1 || true
}
# Waits for the step's result file to hold its result line, then records it.
step_end() {
	name=$1
	limit=$2
	tries=0
	line=
	while [ "$tries" -lt "$limit" ]; do
		serial "cat /tmp/$name.out" > "$out/$name.txt" 2>&1 || true
		line=$(grep -E '^[a-z]+: (PASS|FAIL)' "$out/$name.txt" | tail -1 || true)
		[ -z "$line" ] || break
		tries=$((tries + 1))
		sleep 2
	done
	[ -n "$line" ] || line="$name: FAIL no result"
	say "$line"
	case $line in
	*PASS*) ;;
	*) failed=1 ;;
	esac
}
# Waits for the program's marker file, so a change comes after the registration.
marker_wait() {
	tries=0
	while ! serial "test -e $1" >/dev/null 2>&1; do
		tries=$((tries + 1))
		[ "$tries" -lt 60 ] || return 1
		sleep 0.5
	done
}

# Lets a gated step register: waits for its program's open (NAME.ready), then says go (NAME.go).
gate_go() {
	marker_wait "/tmp/$1.ready" || { say "$1: FAIL no ready"; failed=1; return 0; }
	serial "touch /tmp/$1.go" > /dev/null 2>&1 || true
}
# The topology sequence a step's result line ends with ("sequence A -> B"): first A, last B.
sequence_of() {
	sed -n 's/.* sequence \([0-9]*\) -> \([0-9]*\).*/\1 \2/p' "$out/$1.txt" | tail -1
}
# quiet: the sequence the next step read first is the one the step before ended with.
quiet_between() {
	last=$(sequence_of "$1" | cut -d' ' -f2)
	first=$(sequence_of "$2" | cut -d' ' -f1)
	if [ -n "$last" ] && [ "$last" = "$first" ]; then
		say "quiet: PASS $1 -> $2 sequence $last"
	else
		say "quiet: FAIL $1 ended at '$last', $2 began at '$first' with no head change between"
		failed=1
	fi
}

# idle: nothing changes for 30 s.
step_start idle idle 30
sleep 30
step_end idle 30

# wait: plug, then unplug; each registration wakes at the change.
for change in 1024x768 off; do
	step_start "wait-$change" wait 20 /tmp/wait-$change.armed /tmp/wait-$change
	gate_go "wait-$change"
	marker_wait /tmp/wait-$change.armed || { say "wait-$change: FAIL no marker"; failed=1; }
	head1 "$change"
	step_end "wait-$change" 20
done

# latency: the host waits 2 s after seeing the marker, then plugs the head.  The program's time from its marker,
# less the host's time from seeing the marker to starting the change, bounds the delay after the change began
# (the marker was made before the host saw it; the change's D-Bus call is included).
step_start latency wait 20 /tmp/latency.armed /tmp/latency
gate_go latency
marker_wait /tmp/latency.armed || { say "latency: FAIL no marker"; failed=1; }
seen=$(now_ms)
sleep 2
before_change=$(( $(now_ms) - seen ))
head1 1024x768
change_done=$(( $(now_ms) - seen ))
step_end latency 20
woke=$(sed -n 's/.* ms=\([0-9]*\) .*/\1/p' "$out/latency.txt" | tail -1)
if [ -n "$woke" ]; then
	delay=$(( woke - before_change ))
	say "latency: woke ${woke} ms after the marker; the change began ${before_change} ms and ended ${change_done} ms after the host saw the marker; delay <= ${delay} ms"
	if [ "$delay" -gt 1000 ]; then
		say "latency: FAIL more than 1 s after the change began"
		failed=1
	fi
else
	say "latency: FAIL no time"
	failed=1
fi

# cancel: a canceled registration does not wake when the head changes.
step_start cancel cancel 8 /tmp/cancel.armed /tmp/cancel
gate_go cancel
marker_wait /tmp/cancel.armed || { say "cancel: FAIL no marker"; failed=1; }
head1 off
sleep 8
step_end cancel 10

# limit: 16 per open, ENOSPC past them, ENODEV after the close, 16 after reopening.
step_start limit limit
step_end limit 10

# race: registrations back to back while the head is plugged and unplugged.
step_start race race 600 5 /tmp/race
gate_go race
for change in 1024x768 off 800x600 off 1024x768 off 800x600 off; do
	head1 "$change"
	sleep 0.3
done
step_end race 30

# No step's program moved the sequence by opening the GPU: only the head changes did.
quiet_between idle wait-1024x768
quiet_between wait-1024x768 wait-off
quiet_between wait-off latency
quiet_between latency cancel
quiet_between cancel race

# The guest's kernel lines about the GPU, fetched with a command.
serial 'dmesg | grep -i -e venus -e gpu' > "$out/dmesg-gpu.txt" 2>&1 || true

if [ "$failed" -eq 0 ]; then
	say "qemu-display-fence: PASS"
else
	say "qemu-display-fence: FAIL"
	exit 1
fi

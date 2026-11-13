#!/bin/sh
# The compositor's event loop woken by events, not by a 10 ms pace, on the QEMU Venus guest: builds the image
# (plan/ws353/tests/config-amd64-mainwake.mk), boots it, starts the compositor from the serial line with its log in a
# guest file, and judges by counting the compositor's KWL lines in that file with commands on the serial line (the
# console's text is never read for the verdict).
#
# Steps: the start (the timer thread in KWL THREADS, the capture listening); 30 s idle (the KWL PERF windows over it:
# at most two passes in ten seconds and no timeout, where a 10 ms pace makes a thousand of each: the PING's pass
# and the clock's minute, on its timer); the
# wallpaper changed twice with keiland-settings (the loader's end wakes the loop: each picture shown within 30 s);
# two captures with keiland-shot at once (the second waits for the first's sending, whose end wakes the loop: both
# written within 30 s); 20 s idle again; a USB keyboard plugged with QMP (taken from the system's event, the time from
# the plug to the compositor's line measured) and unplugged (closed); 20 s idle again; SIGTERM (the timer thread
# stopped, KWL EXIT within 20 s); no KWL FAILED.
#
#   sh plan/ws353/tests/qemu-main-wake.sh [OUT]
#
# OUT (default build/ws353-p005-qemu) gets summary.txt, the make logs and the guest's log lines.  The guest's runtime
# is build/ws353-p005-run.  The image is build/x86_64/hdd-image.img (the build keeps the x86_64 sysroot in build/x86_64
# whatever BUILD says).  IMAGE=<file> skips the build.
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
set -eu
cd "$(dirname -- "$0")/../../.."
out=${1:-build/ws353-p005-qemu}
config=plan/ws353/tests/config-amd64-mainwake.mk
export GUEST_RUNTIME=$PWD/build/ws353-p005-run
# The host's Venus renderer (plan/ws353/tests/build-venus-renderer.sh makes it), on Lavapipe with zink for QEMU's GL.
VENUS_RENDERER=${VENUS_RENDERER:-$PWD/build/ws035-sq-venus/install}
export LIBGL_ALWAYS_SOFTWARE=1
export MESA_LOADER_DRIVER_OVERRIDE=zink
export VK_DRIVER_FILES="${VK_DRIVER_FILES:-/usr/share/vulkan/icd.d/lvp_icd.json}"
export RENDER_SERVER_EXEC_PATH="${RENDER_SERVER_EXEC_PATH:-$VENUS_RENDERER/libexec/virgl_render_server}"
export LD_LIBRARY_PATH="$VENUS_RENDERER/lib/x86_64-linux-gnu${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
if [ ! -x "$RENDER_SERVER_EXEC_PATH" ]; then
	echo "qemu-main-wake: FAIL no Venus renderer at $RENDER_SERVER_EXEC_PATH (plan/ws353/tests/build-venus-renderer.sh)" >&2
	exit 1
fi
. plan/tools/guest/venus-hostmem.sh
. plan/tools/fresh-out.sh
fresh_out "$out"
summary=$out/summary.txt
: > "$summary"
guest="python3 plan/tools/guest/guest.py"
failed=0

say() {
	echo "$*" | tee -a "$summary"
}
fail_step() {
	say "$1: FAIL $2"
	failed=1
}

# The image, unless one is given; its build's warnings outside noct (the toolchain's) are a failure.
image=${IMAGE:-}
if [ -z "$image" ]; then
	# The compositor's package stamp does not follow the configuration: the capture's source made newer than the
	# stamp makes the package build again with it.
	touch userland/desktop/wayland/shot.c
	timeout 5400 make -j4 BUILD_JOBS=4 CONFIGMK=$config userland > "$out/make-userland.txt" 2>&1
	timeout 3000 make -j4 BUILD_JOBS=4 CONFIGMK=$config disk > "$out/make-disk.txt" 2>&1
	sync
	warnings=$(cat "$out/make-userland.txt" "$out/make-disk.txt" | grep 'warning:' | grep -vc 'userland/base/noct/' || true)
	say "build: warnings outside noct ${warnings}"
	[ "$warnings" -eq 0 ] || failed=1
	image=build/x86_64/hdd-image.img
fi

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
timeout 900 $guest start "$image" --cpus 4 --qemu-extra \
    "-object memory-backend-memfd,id=mem,size=8G,share=on -machine memory-backend=mem -device virtio-gpu-gl-pci,id=venus,venus=on,blob=on,hostmem=$VENUS_HOSTMEM,max_outputs=1 -display dbus,gl=on,rendernode=/dev/dri/renderD128" \
    > "$out/start.txt"

# One short command on the serial line (it logs in first when needed); a garbled status costs 20 s, not 120.
serial() {
	timeout 30 python3 plan/tools/guest/serial.py --socket "$GUEST_RUNTIME/serial.sock" --timeout 20 run "$1"
}
tries=0
until serial 'uname -a' > "$out/uname.txt" 2>&1; do
	tries=$((tries + 1))
	if [ "$tries" -ge 12 ]; then
		say "qemu-main-wake: FAIL no shell on the serial line (see $out/uname.txt)"
		exit 1
	fi
	sleep 10
done
now_ms() {
	date +%s%3N
}

# Sends one QMP command (its arguments as JSON); events QEMU sends meanwhile are skipped.
qmp() {
	timeout 40 python3 -I -c '
import json, socket, sys
connection = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
connection.settimeout(30)
connection.connect(sys.argv[1])
stream = connection.makefile("rw")
stream.readline()
def ask(request):
	stream.write(json.dumps(request) + "\n")
	stream.flush()
	while True:
		message = json.loads(stream.readline())
		if "event" not in message:
			return message
ask({"execute": "qmp_capabilities"})
answer = ask({"execute": sys.argv[2], "arguments": json.loads(sys.argv[3])})
print(json.dumps(answer))
sys.exit(1 if "error" in answer else 0)
' "$GUEST_RUNTIME/qmp.sock" "$1" "$2" >> "$out/qmp.txt" 2>&1
}

# How many lines of a guest file match: the last all-digit line of grep -c (a kernel record may land in the serial
# output), tried again when none came.
dir=/tmp/p5
log=$dir/kwl.log
count_in() {
	tries=0
	while [ "$tries" -lt 4 ]; do
		number=$(serial "grep -c '$1' $2" 2>/dev/null | tr -d '\r' | grep -E '^[0-9]+$' | sed -n '$p' || true)
		if [ -n "$number" ]; then
			echo "$number"
			return 0
		fi
		tries=$((tries + 1))
		sleep 1
	done
	echo -1
}
count() {
	count_in "$1" "$log"
}

# Waits until at least MIN lines of a guest file match, at most LIMIT seconds; the check is the command's status.
wait_in() {
	started=$(date +%s)
	while ! serial "test \$(grep -c '$1' $4) -ge $2" >/dev/null 2>&1; do
		if [ $(( $(date +%s) - started )) -ge "$3" ]; then
			return 1
		fi
		sleep 1
	done
}
wait_count() {
	wait_in "$1" "$2" "$3" "$log"
}

# The KWL PERF windows after line FROM of the log, as "ms=M passes=P timeouts=T lines=N" (the guest's awk sums them).
rate() {
	tries=0
	while [ "$tries" -lt 4 ]; do
		line=$(serial "sh /root/perf-rate.sh $1 $log" 2>/dev/null | tr -d '\r' | grep -E '^ms=[0-9]+ passes=[0-9]+ timeouts=[0-9]+ lines=[0-9]+$' | sed -n '$p' || true)
		if [ -n "$line" ]; then
			echo "$line"
			return 0
		fi
		tries=$((tries + 1))
		sleep 1
	done
	echo "ms=0 passes=0 timeouts=0 lines=0"
}

# An idle stretch of SECONDS: its KWL PERF windows (printed by the pass a capture's PING brings after it) hold at most
# two passes in ten seconds and no timeout at all (the PING's pass, and the clock's minute, which is its timer's
# pass; a 10 ms pace makes a thousand of each).  The window under way is ended first (a PING's pass, five seconds after it began), so that the windows
# summed hold only the idle stretch; they cover it but for the last one when a pass printed a window less than five
# seconds before the last PING.
idle() {
	name=$1
	sleep 6
	serial "keiland-shot --socket $dir/keiland-shot.sock --request PING > $dir/ping.txt 2>&1" > /dev/null 2>&1 || true
	sleep 1
	from=$(count '')
	sleep "$2"
	serial "keiland-shot --socket $dir/keiland-shot.sock --request PING > $dir/ping.txt 2>&1" > /dev/null 2>&1 || true
	sleep 2
	numbers=$(rate "$from")
	ms=$(echo "$numbers" | sed 's/^ms=\([0-9]*\) .*/\1/')
	passes=$(echo "$numbers" | sed 's/.* passes=\([0-9]*\) .*/\1/')
	timeouts=$(echo "$numbers" | sed 's/.* timeouts=\([0-9]*\) .*/\1/')
	if [ "$ms" -ge $(( ($2 - 5) * 1000 )) ] && [ $(( passes * 10000 )) -le $(( ms * 2 )) ] && [ "$timeouts" -eq 0 ]; then
		say "$name: PASS $numbers"
	else
		fail_step "$name" "$numbers (wanted ms>=$(( ($2 - 5) * 1000 )), passes<=2 in 10 s and timeouts 0)"
	fi
}

# The compositor, in the background, its log in a guest file.
serial "rm -rf $dir; mkdir -p $dir/home; XDG_RUNTIME_DIR=$dir HOME=$dir/home /bin/wayland --testing --glass --timeout=1800 > $log 2>&1 < /dev/null &" \
    > "$out/compositor-start.txt" 2>&1 || true

# The start: window mode, the timer thread running, the capture listening.
if wait_count 'KWL MODE window' 1 120 && wait_count 'KWL THREADS .*timer' 1 30 && wait_count 'KWL SHOT listening' 1 30; then
	say "start: PASS window mode, the timer thread in KWL THREADS, the capture listening"
else
	fail_step start "no window mode, timer thread or capture (see $out/kwl.log)"
fi
sleep 10

# Idle.
idle idle-30 30

# The wallpaper, twice: the loader's thread reads it and its end wakes the loop, which shows it.
for picture in 1 2; do
	shown=$(count "KWL GLASS wallpaper path=/root/wall-$picture.png")
	asked_at=$(now_ms)
	serial "WAYLAND_DISPLAY=/tmp/wayland-0 XDG_RUNTIME_DIR=$dir keiland-settings --timeout-ms=20000 set wallpaper /root/wall-$picture.png > $dir/set-$picture.txt 2>&1 < /dev/null &" \
	    > /dev/null 2>&1 || true
	if wait_count "KWL GLASS wallpaper path=/root/wall-$picture.png" $((shown + 1)) 30; then
		say "wallpaper-$picture: PASS shown within $(( $(now_ms) - asked_at )) ms of the ask (to the serial check's resolution)"
	else
		fail_step "wallpaper-$picture" "not shown within 30 s"
	fi
	serial "cat $dir/set-$picture.txt" > "$out/set-$picture.txt" 2>&1 || true
done

# Two captures at once: the second waits for the first's sending, whose end wakes the loop.
sent=$(count 'KWL SHOT sent')
serial "keiland-shot --socket $dir/keiland-shot.sock $dir/a.png > $dir/shot-a.txt 2>&1 < /dev/null & keiland-shot --socket $dir/keiland-shot.sock $dir/b.png > $dir/shot-b.txt 2>&1 < /dev/null &" \
    > /dev/null 2>&1 || true
if wait_count 'KWL SHOT sent' $((sent + 2)) 30 && wait_in 'KEILAND-SHOT /tmp/p5/a.png' 1 10 $dir/shot-a.txt && wait_in 'KEILAND-SHOT /tmp/p5/b.png' 1 10 $dir/shot-b.txt; then
	say "shot: PASS two captures written"
else
	fail_step shot "the two captures were not both written within 30 s"
fi

# Idle after them: no pace left behind.
idle idle-after-20 20

# A USB keyboard plugged: the system's event asks for the scan, which takes it; unplugged: it closes.
keyboards=$(count 'KWL INPUT device=.*kind=keyboard')
closed=$(count 'KWL INPUT_CLOSED')
plugged_at=$(now_ms)
if qmp device_add '{"driver": "usb-kbd", "bus": "xhci.0", "id": "p5kbd"}' && wait_count 'KWL INPUT device=.*kind=keyboard' $((keyboards + 1)) 20; then
	say "plug: PASS the keyboard taken within $(( $(now_ms) - plugged_at )) ms of the plug (to the serial check's resolution)"
else
	fail_step plug "the keyboard was not taken within 20 s"
fi
if qmp device_del '{"id": "p5kbd"}' && wait_count 'KWL INPUT_CLOSED' $((closed + 1)) 20; then
	say "unplug: PASS the keyboard closed"
else
	fail_step unplug "the keyboard was not closed within 20 s"
fi

# Idle after the plug: the quick scans after the unplug's event (two seconds) have ended.
sleep 6
idle idle-after-plug-20 20

# The end: SIGTERM, the timer thread stopped, the compositor gone within 20 s.
serial 'for p in $(ps -A -o pid,args | grep "[/]bin/wayland" | while read a b; do echo $a; done); do kill $p; done' \
    > "$out/kill.txt" 2>&1 || true
if wait_count 'KWL TIMER stopped' 1 20 && wait_count 'KWL EXIT' 1 20; then
	say "stop: PASS the timer thread stopped and the compositor ended"
else
	fail_step stop "no KWL TIMER stopped or KWL EXIT within 20 s"
fi
failures=$(count 'KWL FAILED')
if [ "$failures" -eq 0 ]; then
	say "failed: PASS no KWL FAILED"
else
	fail_step failed "${failures} KWL FAILED lines"
fi

# The guest's log for the record (read over the serial line: kernel records may be mixed in; not used for the verdict).
serial "grep -e 'KWL PERF [0-9]' -e 'KWL MODE' -e 'KWL THREADS' -e 'KWL TIMER' -e 'KWL GLASS' -e 'KWL PREFERENCES' -e 'KWL SHOT' -e 'KWL INPUT' -e 'KWL EVENT' -e 'KWL EXIT' -e 'KWL FAILED' $log" > "$out/kwl.log" 2>&1 || true

if [ "$failed" -eq 0 ]; then
	say "qemu-main-wake: PASS"
else
	say "qemu-main-wake: FAIL"
	exit 1
fi

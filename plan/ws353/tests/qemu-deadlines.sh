#!/bin/sh
# The compositor's deadlines on timers, on the QEMU Venus guest: builds the image
# (plan/ws353/tests/config-amd64-deadlines.mk: qemu-main-wake.sh's, with keiland-system and a stand-in input method that
# ends at once), boots it, starts the compositor from the serial line with its log in a guest file, and judges by
# counting the compositor's KWL lines in that file with commands on the serial line (the console's text is never read
# for the verdict).
#
# Steps: the start (the timer thread in KWL THREADS); the desktop program ("sleep 1") started again by its timer
# (the times the log gives, worked out by the guest's awk: from each end to the next start at least 2000 ms and at
# most 2600 ms, four starts, then the minute's limit); the input method lost and started again by its timer until it
# is given up (three starts); 30 s idle (at most two passes and one timeout in ten seconds: no deadline leaves a
# pace behind); the system monitor followed for 5 s (its samples at the period from the timer) and the objects' count dropping to 0
# after the client went (the 250 ms look, on the timer while objects are there); 20 s idle again (that look stopped:
# the same bounds as the first idle); a partial capture whose frame never comes, ended by its timer with the timeout's
# error; SIGTERM; then a second compositor with --timeout=10 that ends by its timer (KWL TIMEOUT, KWL EXIT, not before
# 10 s, the timer late by at most 100 ms); no KWL FAILED.
#
#   sh plan/ws353/tests/qemu-deadlines.sh [OUT]
#
# OUT (default build/ws353-p006-qemu) gets summary.txt, the make logs and the guest's log lines.  The guest's runtime
# is build/ws353-p006-run.  The image is build/x86_64/hdd-image.img.  IMAGE=<file> skips the build.
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
set -eu
cd "$(dirname -- "$0")/../../.."
out=${1:-build/ws353-p006-qemu}
config=plan/ws353/tests/config-amd64-deadlines.mk
export GUEST_RUNTIME=$PWD/build/ws353-p006-run
# The host's Venus renderer (plan/ws353/tests/build-venus-renderer.sh makes it), on Lavapipe with zink for QEMU's GL.
VENUS_RENDERER=${VENUS_RENDERER:-$PWD/build/ws035-sq-venus/install}
export LIBGL_ALWAYS_SOFTWARE=1
export MESA_LOADER_DRIVER_OVERRIDE=zink
export VK_DRIVER_FILES="${VK_DRIVER_FILES:-/usr/share/vulkan/icd.d/lvp_icd.json}"
export RENDER_SERVER_EXEC_PATH="${RENDER_SERVER_EXEC_PATH:-$VENUS_RENDERER/libexec/virgl_render_server}"
export LD_LIBRARY_PATH="$VENUS_RENDERER/lib/x86_64-linux-gnu${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
if [ ! -x "$RENDER_SERVER_EXEC_PATH" ]; then
	echo "qemu-deadlines: FAIL no Venus renderer at $RENDER_SERVER_EXEC_PATH (plan/ws353/tests/build-venus-renderer.sh)" >&2
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
		say "qemu-deadlines: FAIL no shell on the serial line (see $out/uname.txt)"
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
dir=/tmp/p6
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
			# One more look: a garbled serial check may have used up the time.
			serial "test \$(grep -c '$1' $4) -ge $2" >/dev/null 2>&1
			return
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
# two passes and one timeout in ten seconds (the PING's pass, and the clock's minute; a 10 ms pace makes a thousand
# of each).  The window under way is ended first (a PING's pass, five seconds after it began), so that the windows
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
	if [ "$ms" -ge $(( ($2 - 5) * 1000 )) ] && [ $(( passes * 10000 )) -le $(( ms * 2 )) ] && [ $(( timeouts * 10000 )) -le $(( ms * 1 )) ]; then
		say "$name: PASS $numbers"
	else
		fail_step "$name" "$numbers (wanted ms>=$(( ($2 - 5) * 1000 )), passes<=2 and timeouts<=1 in 10 s)"
	fi
}

# The compositor, in the background, its log in a guest file; its desktop program ends after a second.
serial "rm -rf $dir; mkdir -p $dir/home; XDG_RUNTIME_DIR=$dir HOME=$dir/home /bin/wayland --testing --glass --timeout=1800 --desktop-client='sleep 1' > $log 2>&1 < /dev/null &" \
    > "$out/compositor-start.txt" 2>&1 || true

# The start: window mode, the timer thread running.
if wait_count 'KWL MODE window' 1 120 && wait_count 'KWL THREADS .*timer' 1 30 && wait_count 'KWL SHOT listening' 1 30; then
	say "start: PASS window mode, the timer thread in KWL THREADS, the capture listening"
else
	fail_step start "no window mode, timer thread or capture (see $out/kwl.log)"
fi

# The desktop program: started again two seconds after each end (the timer), four times in the minute, then the limit.
# The guest's awk takes the times the log gives: each end's (when the program was collected) to the next start's.
if wait_count 'KWL DESKTOP start-limit' 1 60; then
	gaps=$(serial "awk '/KWL DESKTOP exited pid=/ { split(\$NF, a, \"=\"); gone = a[2] } /KWL DESKTOP start pid=/ { split(\$NF, a, \"=\"); n++; if (gone != \"\") { g = a[2] - gone; if (m == 0 || g < low) low = g; if (g > high) high = g; m++ } } END { printf \"starts=%d restarts=%d low=%d high=%d\\n\", n, m, low, high }' $log" \
	    2>/dev/null | tr -d '\r' | grep -E '^starts=[0-9]+ restarts=[0-9]+ low=[0-9]+ high=[0-9]+$' | sed -n '$p' || true)
	starts=$(echo "$gaps" | sed -n 's/^starts=\([0-9]*\) .*/\1/p')
	restarts=$(echo "$gaps" | sed -n 's/.* restarts=\([0-9]*\) .*/\1/p')
	low=$(echo "$gaps" | sed -n 's/.* low=\([0-9]*\) .*/\1/p')
	high=$(echo "$gaps" | sed -n 's/.* high=\([0-9]*\)$/\1/p')
	if [ -n "$starts" ] && [ "$starts" -eq 4 ] && [ "$restarts" -eq 3 ] && [ "$low" -ge 2000 ] && [ "$high" -le 2600 ]; then
		say "desktop-restart: PASS $gaps (ms from an end to the next start: the 2 s timer and the start's report)"
	else
		fail_step desktop-restart "'$gaps' (wanted starts=4 restarts=3, 2000<=low, high<=2600)"
	fi
else
	fail_step desktop-restart "no KWL DESKTOP start-limit within 60 s"
fi

# The input method: lost at once, started again by its timer, given up after three starts.
if wait_count 'KWL IME given-up' 1 60; then
	started=$(count 'KWL IME started pid=')
	lost=$(count 'KWL IME lost')
	if [ "$started" -eq 3 ] && [ "$lost" -eq 3 ]; then
		say "ime-restart: PASS started=$started lost=$lost, then given up"
	else
		fail_step ime-restart "started=$started lost=$lost (wanted 3 and 3)"
	fi
else
	fail_step ime-restart "no KWL IME given-up within 60 s"
fi
sleep 5

# Idle.
idle idle-30 30

# The system monitor for 5 s: its samples come at the period; after the client, the objects' count drops to 0.
frames_before=$(count 'KWL SYSTEM monitor subscribers=0')
serial "WAYLAND_DISPLAY=/tmp/wayland-0 XDG_RUNTIME_DIR=$dir keiland-system --timeout-ms=5000 monitor > $dir/monitor.txt 2>&1 < /dev/null" \
    > /dev/null 2>&1 || true
frames=$(count_in 'KEILAND-SYSTEM monitor-frame' $dir/monitor.txt)
if [ "$frames" -ge 12 ] && [ "$frames" -le 24 ]; then
	say "monitor: PASS frames=$frames in 5 s (period 250 ms)"
else
	fail_step monitor "frames=$frames in 5 s (wanted 12..24)"
fi
if wait_count 'KWL SYSTEM monitor subscribers=0' $((frames_before + 1)) 10; then
	say "monitor-gone: PASS the objects' count dropped to 0"
else
	fail_step monitor-gone "no KWL SYSTEM monitor subscribers=0 within 10 s"
fi
sleep 3

# Idle after the monitor: its look stopped with the objects.
idle idle-after-monitor-20 20

# A partial capture whose frame never comes (nothing changes): its timer ends it with the timeout's error.
errors=$(count 'KWL SHOT error=timeout')
asked_at=$(now_ms)
serial "keiland-shot --socket $dir/keiland-shot.sock --timeout 15000 --partial $dir/p.png > $dir/shot-p.txt 2>&1 < /dev/null &" > /dev/null 2>&1 || true
if wait_count 'KWL SHOT error=timeout' $((errors + 1)) 15; then
	took=$(( $(now_ms) - asked_at ))
	if [ "$took" -ge 3000 ]; then
		say "shot-timeout: PASS ended with the timeout's error ${took} ms after the ask (to the serial check's resolution)"
	else
		fail_step shot-timeout "ended ${took} ms after the ask (wanted 3000 or more)"
	fi
else
	fail_step shot-timeout "no KWL SHOT error=timeout within 15 s (a frame may have come: see $out/kwl.log)"
fi

# The end: SIGTERM, the timer thread stopped, the compositor gone within 20 s.
serial 'for p in $(ps -A -o pid,args | grep "[/]bin/wayland" | while read a b; do echo $a; done); do kill $p; done' \
    > "$out/kill.txt" 2>&1 || true
if wait_count 'KWL TIMER stopped' 1 30 && wait_count 'KWL EXIT' 1 30; then
	say "stop: PASS the timer thread stopped and the compositor ended"
else
	fail_step stop "no KWL TIMER stopped or KWL EXIT within 30 s"
fi
failures=$(count 'KWL FAILED')
serial "grep -e 'KWL PERF [0-9]' -e 'KWL MODE' -e 'KWL THREADS' -e 'KWL TIMER' -e 'KWL DESKTOP' -e 'KWL IME' -e 'KWL SYSTEM monitor' -e 'KWL SHOT' -e 'KWL EXIT' -e 'KWL FAILED' $log" > "$out/kwl.log" 2>&1 || true
sleep 3

# A run bounded to 10 s: its timer ends it.
bounded=$dir/bounded.log
started_at=$(now_ms)
serial "XDG_RUNTIME_DIR=$dir HOME=$dir/home /bin/wayland --testing --glass --timeout=10 --desktop-client=none > $bounded 2>&1 < /dev/null &" \
    > /dev/null 2>&1 || true
if wait_in 'KWL EXIT' 1 60 $bounded; then
	took=$(( $(now_ms) - started_at ))
	late=$(serial "awk '/^KWL TIMEOUT after_ms=10000 late_ms=/ { split(\$4, a, \"=\"); print \"late=\" a[2] }' $bounded" 2>/dev/null | tr -d '\r' | grep -E '^late=[0-9]+$' | sed -n '$p' | sed 's/late=//' || true)
	if [ -n "$late" ] && [ "$late" -le 100 ] && [ "$took" -ge 10000 ]; then
		say "timeout: PASS KWL TIMEOUT after_ms=10000 late_ms=$late, KWL EXIT ${took} ms after the start (to the serial check's resolution)"
	else
		fail_step timeout "late_ms='$late', ended ${took} ms after the start (wanted late_ms<=100, 10000 or more)"
	fi
else
	fail_step timeout "the bounded run did not end within 60 s"
fi
bounded_failures=$(count_in 'KWL FAILED' $bounded)
serial "grep -e 'KWL READY' -e 'KWL TIMEOUT' -e 'KWL TIMER' -e 'KWL EXIT' -e 'KWL FAILED' $bounded" > "$out/bounded.log" 2>&1 || true
if [ "$failures" -eq 0 ] && [ "$bounded_failures" -eq 0 ]; then
	say "failed: PASS no KWL FAILED"
else
	fail_step failed "${failures} and ${bounded_failures} KWL FAILED lines"
fi

if [ "$failed" -eq 0 ]; then
	say "qemu-deadlines: PASS"
else
	say "qemu-deadlines: FAIL"
	exit 1
fi

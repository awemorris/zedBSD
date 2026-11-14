#!/bin/sh
# The compositor's animations on frames and its clock on a timer, on the QEMU Venus guest: builds the image
# (plan/ws353/tests/config-amd64-anim.mk: qemu-ui-deadlines.sh's, with keiland-notify), boots it, starts the compositor
# (glass, 1280x800, --log-frames) from the serial line with its log in a guest file, drives it with aat-input and
# keiland-notify, and judges by the compositor's KWL lines in that file, read with commands on the serial line (the
# console's text is never read for the verdict).  The animations' times are the compositor's own (at_ms in its lines).
#
# Steps: the start; 60 s idle (the KWL PERF windows: at most two passes in ten seconds and no timeout at all; the
# timers fired, by name, are recorded: the clock's minute among them); App Home opened with the Super key (KWL HOME
# open via=super to KWL HOME opened, 280 ms of animation: 272 to 680 ms, and at least three frames drawn) and closed
# again (240 ms: 232 to 640 ms; the animation's first frame shows it 8 ms on, so a frame recorded within 8 ms of the
# request ends it up to 8 ms before its length); a notification's board (keiland-notify) entering, staying and leaving (KWL NOTIFY
# show to gone, 280 + 3000 + 280 ms: 3560 to 3960 ms); the wall clock stepped ten minutes on and then back with date
# (each time the clock's timer is armed again within 61 s, and the next arming after that comes just after a minute
# turned: KWL CLOCK arm into_ms at most 1000); the on-screen keyboard slid out of its corner (KWL OSK slide end
# leaving=0: at least three frames, no gap between two over 200 ms); 20 s idle again (as the first); SIGTERM; no KWL
# FAILED.
#
#   sh plan/ws353/tests/qemu-animation.sh [OUT]
#
# OUT (default build/ws353-p008-qemu) gets summary.txt, the make logs and the guest's log lines.  The guest's runtime
# is build/ws353-p008-run.  The image is build/x86_64/hdd-image.img.  IMAGE=<file> skips the build.
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
set -eu
cd "$(dirname -- "$0")/../../.."
out=${1:-build/ws353-p008-qemu}
config=plan/ws353/tests/config-amd64-anim.mk
export GUEST_RUNTIME=$PWD/build/ws353-p008-run
# The host's Venus renderer (plan/ws353/tests/build-venus-renderer.sh makes it), on Lavapipe with zink for QEMU's GL.
VENUS_RENDERER=${VENUS_RENDERER:-$PWD/build/ws035-sq-venus/install}
export LIBGL_ALWAYS_SOFTWARE=1
export MESA_LOADER_DRIVER_OVERRIDE=zink
export VK_DRIVER_FILES="${VK_DRIVER_FILES:-/usr/share/vulkan/icd.d/lvp_icd.json}"
export RENDER_SERVER_EXEC_PATH="${RENDER_SERVER_EXEC_PATH:-$VENUS_RENDERER/libexec/virgl_render_server}"
export LD_LIBRARY_PATH="$VENUS_RENDERER/lib/x86_64-linux-gnu${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
if [ ! -x "$RENDER_SERVER_EXEC_PATH" ]; then
	echo "qemu-animation: FAIL no Venus renderer at $RENDER_SERVER_EXEC_PATH (plan/ws353/tests/build-venus-renderer.sh)" >&2
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
	# The package and kernel stamps do not follow the configuration: the capture's source and the file the input
	# injector's switch is read in made newer than their stamps build them again with it.
	touch userland/desktop/wayland/shot.c src/kern/vfs.c
	timeout 5400 make -j4 BUILD_JOBS=4 CONFIGMK=$config userland > "$out/make-userland.txt" 2>&1
	timeout 3000 make -j4 BUILD_JOBS=4 CONFIGMK=$config disk > "$out/make-disk.txt" 2>&1
	sync
	warnings=$(cat "$out/make-userland.txt" "$out/make-disk.txt" | grep 'warning:' | grep -vc 'userland/base/noct/' || true)
	say "build: warnings outside noct ${warnings}"
	[ "$warnings" -eq 0 ] || failed=1
	image=build/x86_64/hdd-image.img
fi

# The guest: Venus with one head on a private D-Bus display, the renderer the host has.
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
		say "qemu-animation: FAIL no shell on the serial line (see $out/uname.txt)"
		exit 1
	fi
	sleep 10
done
now_ms() {
	date +%s%3N
}

# How many lines of a guest file match: the last all-digit line of grep -c (a kernel record may land in the serial
# output), tried again when none came.
dir=/tmp/p8
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

# The value of NAME= in the last log line matching PATTERN (the guest's awk prints it alone on a line).
field() {
	tries=0
	while [ "$tries" -lt 4 ]; do
		value=$(serial "awk '/$1/ { for (i = 1; i <= NF; i++) if (index(\$i, \"$2=\") == 1) v = substr(\$i, length(\"$2\") + 2) } END { print \"value=\" v }' $log" \
		    2>/dev/null | tr -d '\r' | grep -E '^value=' | sed -n '$p' | sed 's/^value=//' || true)
		if [ -n "$value" ]; then
			echo "$value"
			return 0
		fi
		tries=$((tries + 1))
		sleep 1
	done
	echo ""
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

# The timers fired in the KWL PERF windows after line FROM, by name ("timers fired=T NAME=COUNT ...").
timers_after() {
	tries=0
	while [ "$tries" -lt 4 ]; do
		line=$(serial "sh /root/perf-timers.sh $1 $log" 2>/dev/null | tr -d '\r' | grep -E '^timers fired=[0-9]+' | sed -n '$p' || true)
		if [ -n "$line" ]; then
			echo "$line"
			return 0
		fi
		tries=$((tries + 1))
		sleep 1
	done
	echo "timers fired=?"
}

# An idle stretch of SECONDS, its KWL PERF windows (printed by the pass a capture's PING brings after it) judged
# against PASSES_PER_10S and TIMEOUTS_PER_10S (passes and timeouts in ten seconds at most) when given, and the timers
# that fired in it recorded by name.  The windows cover the stretch but for the last one when a pass printed a window
# less than five seconds before the PING (a window is printed five seconds after it began, at a pass).
idle() {
	name=$1
	# The window under way is ended first (a PING's pass, five seconds after it began), so that the windows summed
	# hold only the idle stretch.
	sleep 6
	serial "keiland-shot --socket $dir/keiland-shot.sock --request PING > $dir/ping.txt 2>&1" > /dev/null 2>&1 || true
	sleep 1
	from=$(count '')
	sleep "$2"
	serial "keiland-shot --socket $dir/keiland-shot.sock --request PING > $dir/ping.txt 2>&1" > /dev/null 2>&1 || true
	sleep 2
	numbers=$(rate "$from")
	timers=$(timers_after "$from")
	ms=$(echo "$numbers" | sed 's/^ms=\([0-9]*\) .*/\1/')
	passes=$(echo "$numbers" | sed 's/.* passes=\([0-9]*\) .*/\1/')
	timeouts=$(echo "$numbers" | sed 's/.* timeouts=\([0-9]*\) .*/\1/')
	if [ -z "${3:-}" ]; then
		say "$name: RECORD $numbers; $timers"
	elif [ "$ms" -ge $(( ($2 - 5) * 1000 )) ] && [ $(( passes * 10000 )) -le $(( ms * $3 )) ] && [ $(( timeouts * 10000 )) -le $(( ms * $4 )) ]; then
		say "$name: PASS $numbers; $timers"
	else
		fail_step "$name" "$numbers; $timers (wanted ms>=$(( ($2 - 5) * 1000 )), passes<=$3 and timeouts<=$4 in 10 s)"
	fi
}

# aat-input in the guest (its server holds the mouse, the keyboard and the touch screen).
aat() {
	serial "aat-input $* >> $dir/aat.log 2>&1" > /dev/null 2>&1 || true
}

# Judges a time between two log lines' at_ms (the last of each pattern) against LOW..HIGH milliseconds.
span() {
	name=$1
	begun=$(field "$2" at_ms)
	ended=$(field "$3" at_ms)
	if [ -z "$begun" ] || [ -z "$ended" ]; then
		fail_step "$name" "no at_ms in '$2' or '$3'"
		return
	fi
	took=$(( ended - begun ))
	if [ "$took" -ge "$4" ] && [ "$took" -le "$5" ]; then
		say "$name: PASS ${took} ms$6"
	else
		fail_step "$name" "${took} ms (wanted $4..$5)$6"
	fi
}

# The wall clock stepped by SECONDS (a minute's start ten minutes on or back, date's POSIX form), then the clock's
# timer: armed again within 61 s, and the arming after that just after a minute turned (into_ms at most 1000).
clock_step() {
	name=$1
	armed=$(count 'KWL CLOCK arm')
	serial "date -u \$(date -u -d @\$(( \$(date +%s) + $2 )) +%m%d%H%M%Y) > $dir/date.txt 2>&1" > /dev/null 2>&1 || true
	serial "cat $dir/date.txt" > "$out/$name-date.txt" 2>&1 || true
	wait_count 'KWL CLOCK arm' $((armed + 1)) 75
	first=$(count 'KWL CLOCK arm')
	if [ "$first" -le "$armed" ]; then
		fail_step "$name" "the clock's timer was not armed again within 75 s ($(cat "$out/$name-date.txt" | tr -d '\r' | tail -1))"
		return
	fi
	wait_count 'KWL CLOCK arm' $((armed + 2)) 75
	into=$(field 'KWL CLOCK arm' into_ms)
	if [ "$(count 'KWL CLOCK arm')" -ge $((armed + 2)) ] && [ -n "$into" ] && [ "$into" -le 1000 ]; then
		say "$name: PASS armed again, then $into ms after the minute turned"
	else
		fail_step "$name" "arms $(( $(count 'KWL CLOCK arm') - armed )), the last into_ms='$into' (wanted two, at most 1000)"
	fi
}

# The compositor, in the background, its log in a guest file; no desktop program.
serial "rm -rf $dir; mkdir -p $dir/home; XDG_RUNTIME_DIR=$dir HOME=$dir/home /bin/wayland --testing --glass --log-frames --width=1280 --height=800 --timeout=3000 --desktop-client=none > $log 2>&1 < /dev/null &" \
    > "$out/compositor-start.txt" 2>&1 || true

# The start: window mode, the timer thread running, the capture listening; then the input injector's server.
if wait_count 'KWL MODE window' 1 120 && wait_count 'KWL THREADS .*timer' 1 30 && wait_count 'KWL SHOT listening' 1 30; then
	say "start: PASS window mode, the timer thread in KWL THREADS, the capture listening"
else
	fail_step start "no window mode, timer thread or capture (see $out/kwl.log)"
fi
serial "aat-input start --width 1280 --height 800 > $dir/aat.log 2>&1" > /dev/null 2>&1 || true
wait_count 'KWL LAYER baked what=home inband=0' 1 60
sleep 5

# Idle: two passes in ten seconds at most and no timeout (the PING's pass, and the clock's minute on its timer).
idle idle-60 60 2 0

# App Home opened and closed with the Super key: each animation steps with its frames.
aat move-to 640 600
frames=$(count 'KWL LAT draw')
aat key super
if wait_count 'KWL HOME opened' 1 10; then
	sleep 1
	span home-open 'KWL HOME open via=super' 'KWL HOME opened' 272 680 " ($(( $(count 'KWL LAT draw') - frames )) frames)"
	drawn=$(( $(count 'KWL LAT draw') - frames ))
	[ "$drawn" -ge 3 ] || fail_step home-open-frames "$drawn frames (wanted 3 at least)"
else
	fail_step home-open "no KWL HOME opened within 10 s"
fi
sleep 2
aat key super
if wait_count 'KWL HOME closed' 1 10; then
	span home-close 'KWL HOME close via=super' 'KWL HOME closed' 232 640 ""
else
	fail_step home-close "no KWL HOME closed within 10 s"
fi
sleep 2

# A notification's board enters, stays and leaves, each part on the frames.
serial "WAYLAND_DISPLAY=/tmp/wayland-0 XDG_RUNTIME_DIR=$dir keiland-notify --wait-ms=8000 Animation Board > $dir/notify.txt 2>&1 < /dev/null &" > /dev/null 2>&1 || true
if wait_count 'KWL NOTIFY gone id=' 1 20; then
	span notify-board 'KWL NOTIFY show id=' 'KWL NOTIFY gone id=' 3560 3960 ""
else
	fail_step notify-board "no KWL NOTIFY gone within 20 s (shown: $(count 'KWL NOTIFY show id='))"
fi
sleep 2

# The wall clock stepped on and back: the clock's timer follows it at its next firing.
clock_step clock-forward 600
clock_step clock-back -600

# The on-screen keyboard slides out of its corner on the frames.
aat move-to 640 600
aat drag 6 792 150 650 8
if wait_count 'KWL OSK slide end leaving=0' 1 10; then
	slid=$(field 'KWL OSK slide end leaving=0' frames)
	gap=$(field 'KWL OSK slide end leaving=0' max_gap_ms)
	if [ -n "$slid" ] && [ "$slid" -ge 3 ] && [ -n "$gap" ] && [ "$gap" -le 200 ]; then
		say "osk-slide: PASS $slid frames, the longest gap $gap ms"
	else
		fail_step osk-slide "frames='$slid' max_gap_ms='$gap' (wanted 3 frames at least, gaps 200 ms at most)"
	fi
else
	fail_step osk-slide "no KWL OSK slide end leaving=0 (open: $(count 'KWL OSK open'))"
fi
sleep 3

# Idle again, the keyboard's panel up.
idle idle-after-20 20 2 0

# The end: SIGTERM, the timer thread stopped, the compositor gone.
serial 'for p in $(ps -A -o pid,args | grep "[/]bin/wayland" | while read a b; do echo $a; done); do kill $p; done' \
    > "$out/kill.txt" 2>&1 || true
if wait_count 'KWL TIMER stopped' 1 30 && wait_count 'KWL EXIT' 1 30; then
	say "stop: PASS the timer thread stopped and the compositor ended"
else
	fail_step stop "no KWL TIMER stopped or KWL EXIT within 30 s"
fi
failures=$(count 'KWL FAILED')
if [ "$failures" -eq 0 ]; then
	say "failed: PASS no KWL FAILED"
else
	fail_step failed "${failures} KWL FAILED lines"
fi

# The guest's log for the record (read over the serial line: kernel records may be mixed in; not used for the verdict).
serial "grep -e 'KWL PERF [0-9]' -e 'KWL PERF timers' -e 'KWL MODE' -e 'KWL THREADS' -e 'KWL TIMER' -e 'KWL HOME' -e 'KWL NOTIFY' -e 'KWL CLOCK' -e 'KWL OSK' -e 'KWL EXIT' -e 'KWL FAILED' $log" > "$out/kwl.log" 2>&1 || true

if [ "$failed" -eq 0 ]; then
	say "qemu-animation: PASS"
else
	say "qemu-animation: FAIL"
	exit 1
fi

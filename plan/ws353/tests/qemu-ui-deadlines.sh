#!/bin/sh
# The compositor's UI deadlines on timers, on the QEMU Venus guest: builds the image
# (plan/ws353/tests/config-amd64-ui.mk: qemu-deadlines.sh's, with the test input injector, aat-input and Settings),
# boots it, starts the compositor (glass, 1280x800, --log-frames) from the serial line with its log in a guest file,
# drives it with aat-input, and judges by counting the compositor's KWL lines in that file with commands on the
# serial line (the console's text is never read for the verdict).  In each step nothing but the step's own timer
# wakes the compositor once the input has stopped, so the line that follows shows the timer working.
#
# Steps: the start; Home's picture drawn in the quiet after the start (KWL LAYER baked what=home inband=0, the quiet's
# timer); 30 s idle (the KWL PERF windows: at most two passes in ten seconds and no timeout; the timers fired, by
# name, are recorded); the notifications' log opened with Super+N and closed by its timer after 10 s without input
# (8 to 14 s to the serial checks' resolution); a finger held 1.2 s in the top edge's band is a long press (KWL EDGE
# band hold ms between 500 and 700, the band's timer); Settings started: its window's search field takes the keyboard
# with Ctrl+F and a key held 2 s repeats (KWL TITLEBAR repeat count between 25 and 50: 400 ms delay, then 25 a second,
# on the repeat's timer); the pointer resting on Settings' icon in the bar shows its previews (via=hover), and away
# from them they close (via=leave), each on the bar's timer; the output turned to 90 and back with keiland-system
# (two KWL OUTPUT rotate lines with the rotation; the old frames held for Settings' window and how they ended, from
# KWL OUTPUT rotate shown, are recorded); 30 s idle with Settings open (recorded: the timers by
# name; Settings holds a printers object, printd being in the image, and no printers timer runs for it, see
# qemu-printers.sh); Settings ended and 30 s idle again (as the first); the on-screen keyboard's handwriting
# face: three strokes are recognized 600 ms after the last (KWL OSK hand recognize strokes=3, the recognition's
# timer); SIGTERM; no KWL FAILED.
#
#   sh plan/ws353/tests/qemu-ui-deadlines.sh [OUT]
#
# OUT (default build/ws353-p007-qemu) gets summary.txt, the make logs and the guest's log lines.  The guest's runtime
# is build/ws353-p007-run.  The image is build/x86_64/hdd-image.img.  IMAGE=<file> skips the build.
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
set -eu
cd "$(dirname -- "$0")/../../.."
out=${1:-build/ws353-p007-qemu}
config=plan/ws353/tests/config-amd64-ui.mk
export GUEST_RUNTIME=$PWD/build/ws353-p007-run
# The host's Venus renderer (plan/ws353/tests/build-venus-renderer.sh makes it), on Lavapipe with zink for QEMU's GL.
VENUS_RENDERER=${VENUS_RENDERER:-$PWD/build/ws035-sq-venus/install}
export LIBGL_ALWAYS_SOFTWARE=1
export MESA_LOADER_DRIVER_OVERRIDE=zink
export VK_DRIVER_FILES="${VK_DRIVER_FILES:-/usr/share/vulkan/icd.d/lvp_icd.json}"
export RENDER_SERVER_EXEC_PATH="${RENDER_SERVER_EXEC_PATH:-$VENUS_RENDERER/libexec/virgl_render_server}"
export LD_LIBRARY_PATH="$VENUS_RENDERER/lib/x86_64-linux-gnu${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
if [ ! -x "$RENDER_SERVER_EXEC_PATH" ]; then
	echo "qemu-ui-deadlines: FAIL no Venus renderer at $RENDER_SERVER_EXEC_PATH (plan/ws353/tests/build-venus-renderer.sh)" >&2
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
		say "qemu-ui-deadlines: FAIL no shell on the serial line (see $out/uname.txt)"
		exit 1
	fi
	sleep 10
done
now_ms() {
	date +%s%3N
}

# How many lines of a guest file match: the last all-digit line of grep -c (a kernel record may land in the serial
# output), tried again when none came.
dir=/tmp/p7
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

# Home's picture, drawn in the quiet after the start (the quiet's timer wakes the pass).
if wait_count 'KWL LAYER baked what=home inband=0' 1 60; then
	say "layer-quiet: PASS Home's picture drawn in the quiet ($(count 'KWL LAYER baked what=home inband=0') times)"
else
	fail_step layer-quiet "no KWL LAYER baked what=home inband=0 within 60 s"
fi
sleep 5

# Idle: two passes in ten seconds at most and no timeout (the PING's pass, and the clock's minute on its timer).
idle idle-30 30 2 0

# The notifications' log: opened with Super+N, closed by its timer after 10 s without input.
aat key super+n
if wait_count 'KWL NOTIFY log open' 1 10; then
	opened=$(now_ms)
	if wait_count 'KWL NOTIFY log close reason=idle' 1 25; then
		took=$(( $(now_ms) - opened ))
		if [ "$took" -ge 8000 ] && [ "$took" -le 14000 ]; then
			say "notify-log: PASS closed for no input ${took} ms after it was seen open (to the serial checks' resolution)"
		else
			fail_step notify-log "closed ${took} ms after it was seen open (wanted 8000..14000)"
		fi
	else
		fail_step notify-log "no KWL NOTIFY log close reason=idle within 25 s"
	fi
else
	fail_step notify-log "no KWL NOTIFY log open after Super+N"
fi

# A finger held in the top edge's band is a long press on the band's timer.
serial "aat-input touch-down 0 640 6 >> $dir/aat.log 2>&1; sleep 1.2; aat-input touch-up 0 >> $dir/aat.log 2>&1" > /dev/null 2>&1 || true
if wait_count 'KWL EDGE band hold ms=' 1 10; then
	held=$(field 'KWL EDGE band hold ms=' ms)
	if [ -n "$held" ] && [ "$held" -ge 500 ] && [ "$held" -le 700 ]; then
		say "band-hold: PASS long press after ${held} ms"
	else
		fail_step band-hold "ms='$held' (wanted 500..700)"
	fi
else
	fail_step band-hold "no KWL EDGE band hold (presses: $(count 'KWL EDGE band press'))"
fi
aat key esc
sleep 2

# Settings: its window, the search field's control in the titlebar and its icon in the bar.
program=$(serial 'for p in /bin/settings /usr/bin/settings /bin/keiland-settings-app; do test -x $p && echo "program=$p"; done' 2>/dev/null | tr -d '\r' | grep -E '^program=' | sed -n '1p' | sed 's/^program=//' || true)
say "settings: program '$program'"
serial "WAYLAND_DISPLAY=/tmp/wayland-0 XDG_RUNTIME_DIR=$dir HOME=$dir/home $program > $dir/settings.log 2>&1 < /dev/null &" > /dev/null 2>&1 || true
if wait_count 'KWL APPS icon app=' 1 60 && wait_count 'KWL TITLEBAR control client=' 1 30; then
	say "settings-window: PASS its titlebar's controls and its icon in the bar"
	sleep 5

	# The search field takes the keyboard with Ctrl+F; a key held two seconds repeats on the repeat's timer.
	aat key ctrl+f
	if wait_count 'KWL TITLEBAR focus ' 1 10; then
		serial "aat-input key-down a >> $dir/aat.log 2>&1; sleep 2; aat-input key-up a >> $dir/aat.log 2>&1" > /dev/null 2>&1 || true
		if wait_count 'KWL TITLEBAR repeat key=30 ' 1 30; then
			repeats=$(field 'KWL TITLEBAR repeat key=30 ' count)
			if [ -n "$repeats" ] && [ "$repeats" -ge 25 ] && [ "$repeats" -le 50 ]; then
				say "titlebar-repeat: PASS a held key pressed again $repeats times in 2 s"
			else
				fail_step titlebar-repeat "count='$repeats' (wanted 25..50)"
			fi
		else
			fail_step titlebar-repeat "no KWL TITLEBAR repeat key=30 line"
		fi
		aat key esc
	else
		fail_step titlebar-repeat "the search field did not take the keyboard with Ctrl+F"
	fi
	sleep 2

	# The pointer resting on Settings' icon: its previews show, and close once it has left them.
	icon_x=$(field 'KWL APPS icon app=' x)
	icon_y=$(field 'KWL APPS icon app=' y)
	icon_w=$(field 'KWL APPS icon app=' width)
	icon_h=$(field 'KWL APPS icon app=' height)
	shown=$(count 'KWL APPS preview app=.* via=hover')
	if [ -n "$icon_x" ] && [ -n "$icon_w" ]; then
		aat move-to $(( icon_x + icon_w / 2 )) $(( icon_y + icon_h / 2 ))
		if wait_count 'KWL APPS preview app=.* via=hover' $((shown + 1)) 10; then
			say "apps-hover: PASS the previews shown by the rest on the icon"
			closed=$(count 'KWL APPS preview close via=leave')
			aat move-to 640 600
			if wait_count 'KWL APPS preview close via=leave' $((closed + 1)) 10; then
				say "apps-leave: PASS the previews closed once the pointer left"
			else
				fail_step apps-leave "no KWL APPS preview close via=leave within 10 s"
			fi
		else
			fail_step apps-hover "no KWL APPS preview via=hover within 10 s"
		fi
	else
		fail_step apps-hover "no icon's place"
	fi
	sleep 2

	# The output turned to 90 and back while Settings' window is up: each turn is shown (held for the window or not).
	serial "WAYLAND_DISPLAY=/tmp/wayland-0 XDG_RUNTIME_DIR=$dir keiland-system --timeout-ms=20000 displays > $dir/displays.txt 2>&1" > /dev/null 2>&1 || true
	serial "cat $dir/displays.txt" > "$out/displays.txt" 2>&1 || true
	key=$(sed -n 's/.*KEILAND-SYSTEM display key=\(.*\) label=.*/\1/p' "$out/displays.txt" | tr -d '\r' | sed -n 1p)
	before=$(count 'KWL OUTPUT rotate key=.* rotation=')
	shown_before=$(count 'KWL OUTPUT rotate shown')
	serial "WAYLAND_DISPLAY=/tmp/wayland-0 XDG_RUNTIME_DIR=$dir keiland-system --timeout-ms=20000 display-rotate '$key' 90 > $dir/rotate.txt 2>&1" > /dev/null 2>&1 || true
	sleep 3
	serial "WAYLAND_DISPLAY=/tmp/wayland-0 XDG_RUNTIME_DIR=$dir keiland-system --timeout-ms=20000 display-rotate '$key' 0 >> $dir/rotate.txt 2>&1" > /dev/null 2>&1 || true
	if wait_count 'KWL OUTPUT rotate key=.* rotation=' $((before + 2)) 20; then
		sleep 3
		shown=$(( $(count 'KWL OUTPUT rotate shown') - shown_before ))
		say "rotate: PASS two turns applied; $shown held frames shown (the last: held_ms=$(field 'KWL OUTPUT rotate shown' held_ms) windows=$(field 'KWL OUTPUT rotate shown' windows) late=$(field 'KWL OUTPUT rotate shown' late))"
	else
		fail_step rotate "key='$key', $(count 'KWL OUTPUT rotate key=.* rotation=') turns applied (wanted $((before + 2)))"
	fi
	sleep 5

	# Idle with Settings open: recorded (Settings holds a printers object; nothing about it runs on the clock).
	say "settings-printers: $(count 'KWL PRINTERS object') printers objects made"
	idle idle-settings-30 30

	# Settings ended: the idle is as before.
	serial 'for p in $(ps -A -o pid,args | grep "[/]bin/settings" | while read a b; do echo $a; done); do kill $p; done' > /dev/null 2>&1 || true
	sleep 3
	idle idle-after-settings-30 30 2 0
else
	fail_step settings-window "no titlebar's controls or bar icon (see $out/kwl.log)"
fi

# The handwriting face of the on-screen keyboard: three strokes recognized on the recognition's timer.
aat drag 6 792 150 650 8
sleep 1
aat move-to 1186 486
aat click
sleep 1
aat drag 200 580 400 700 12
aat drag 420 560 402 740 12
aat drag 500 600 650 640 12
if wait_count 'KWL OSK hand recognize strokes=3 ' 1 15; then
	say "osk-hand: PASS three strokes recognized after the last one's wait"
else
	fail_step osk-hand "no KWL OSK hand recognize strokes=3 (open: $(count 'KWL OSK open'), stroke ends: $(count 'KWL OSK hand stroke-end'))"
fi

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
serial "grep -e 'KWL PERF [0-9]' -e 'KWL PERF timers' -e 'KWL MODE' -e 'KWL THREADS' -e 'KWL TIMER' -e 'KWL LAYER baked' -e 'KWL NOTIFY log' -e 'KWL EDGE' -e 'KWL TITLEBAR focus' -e 'KWL TITLEBAR repeat' -e 'KWL APPS' -e 'KWL OUTPUT rotate' -e 'KWL PRINTERS' -e 'KWL OSK' -e 'KWL EXIT' -e 'KWL FAILED' $log" > "$out/kwl.log" 2>&1 || true

if [ "$failed" -eq 0 ]; then
	say "qemu-ui-deadlines: PASS"
else
	say "qemu-ui-deadlines: FAIL"
	exit 1
fi

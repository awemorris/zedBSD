#!/bin/sh
# The printers without a clock, on the QEMU Venus guest: builds the image (plan/ws353/tests/config-amd64-printers.mk:
# qemu-ui-deadlines.sh's with printtest), starts a stand-in LPD printer on the host (plan/ws353/tests/fake-lpd.py, which
# the guest reaches as 10.0.2.2), boots the guest, starts the compositor (glass, 1280x800) from the serial line with its
# log in a guest file, and judges by counting lines in the guest's files with commands on the serial line (the
# console's text is never read for the verdict).
#
# Steps: the start (the printers prepared with the daemon's program); 30 s idle (at most two passes in ten seconds, no
# timeout); Settings opened on its Printers page holds a printers object, and 30 s idle with it open fires no printers
# timer (the KWL PERF timers lines name none; the passes are recorded); printtest adds the host's LPD printer and
# Settings hears the change (ZSETTINGS PRINTERS changed); printtest prints a PDF to it: the job ends done, the
# compositor told its states (KWL PRINTERS job= ... state=4) and Settings heard them, the host's printer took the job;
# 20 s idle while the daemon still runs fires no printers timer; the daemon ends by itself (IDLE after a minute, then
# BYE); the settings file changed behind the compositor's back (as another session would) is not looked at by itself
# (Settings hears nothing for 10 s) and is read when a client makes a new printers object (Settings hears the change;
# the next printtest list shows the printer); printtest removes the host's printer and Settings hears it; Settings
# ended and 30 s idle as the first; SIGTERM; no KWL FAILED.
#
#   sh plan/ws353/tests/qemu-printers.sh [OUT]
#
# OUT (default build/ws353-p019-qemu) gets summary.txt, the make logs, the host printer's log and the guest's log
# lines.  The guest's runtime is build/ws353-p019-run.  The image is build/x86_64/hdd-image.img.  IMAGE=<file> skips the
# build.
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
set -eu
cd "$(dirname -- "$0")/../../.."
out=${1:-build/ws353-p019-qemu}
config=plan/ws353/tests/config-amd64-printers.mk
export GUEST_RUNTIME=$PWD/build/ws353-p019-run
# The host's Venus renderer (plan/ws353/tests/build-venus-renderer.sh makes it), on Lavapipe with zink for QEMU's GL.
VENUS_RENDERER=${VENUS_RENDERER:-$PWD/build/ws035-sq-venus/install}
export LIBGL_ALWAYS_SOFTWARE=1
export MESA_LOADER_DRIVER_OVERRIDE=zink
export VK_DRIVER_FILES="${VK_DRIVER_FILES:-/usr/share/vulkan/icd.d/lvp_icd.json}"
export RENDER_SERVER_EXEC_PATH="${RENDER_SERVER_EXEC_PATH:-$VENUS_RENDERER/libexec/virgl_render_server}"
export LD_LIBRARY_PATH="$VENUS_RENDERER/lib/x86_64-linux-gnu${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
if [ ! -x "$RENDER_SERVER_EXEC_PATH" ]; then
	echo "qemu-printers: FAIL no Venus renderer at $RENDER_SERVER_EXEC_PATH (plan/ws353/tests/build-venus-renderer.sh)" >&2
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

# The host's stand-in LPD printer on a free port, its log in OUT.
lpd_port=$(/usr/bin/python3 -I -c 'import socket; s = socket.socket(); s.bind(("127.0.0.1", 0)); print(s.getsockname()[1]); s.close()')
lpd_log=$PWD/$out/fake-lpd.log
timeout 3600 /usr/bin/python3 -I plan/ws353/tests/fake-lpd.py "$lpd_port" "$lpd_log" &
lpd_pid=$!

# The guest: Venus with one head on a private D-Bus display, the renderer the host has.
mkdir -p "$GUEST_RUNTIME"
bus=$(timeout 10 dbus-daemon --session --fork --print-address=1 --print-pid=1 --nopidfile)
printf '%s\n' "$bus" | sed -n 1p > "$GUEST_RUNTIME/dbus.address"
printf '%s\n' "$bus" | sed -n 2p > "$GUEST_RUNTIME/dbus.pid"
DBUS_SESSION_BUS_ADDRESS=$(cat "$GUEST_RUNTIME/dbus.address")
export DBUS_SESSION_BUS_ADDRESS
finish() {
	kill "$lpd_pid" 2>/dev/null || true
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
		say "qemu-printers: FAIL no shell on the serial line (see $out/uname.txt)"
		exit 1
	fi
	sleep 10
done
now_ms() {
	date +%s%3N
}

# How many lines of a guest file match: the last all-digit line of grep -c (a kernel record may land in the serial
# output), tried again when none came.
dir=/tmp/p19
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

# The number of lines of Settings' log that match.
settings_count() {
	count_in "$1" "$dir/settings.log"
}

# The printers' timers in a "timers fired=..." line (none: empty).
printers_timers() {
	echo "$1" | tr ' ' '\n' | grep '^printers' | tr '\n' ' ' || true
}

# An idle stretch with a client open: recorded, and judged on the printers' timers alone (none may fire).
idle_no_printers() {
	name=$1
	sleep 6
	serial "keiland-shot --socket $dir/keiland-shot.sock --request PING > $dir/ping.txt 2>&1" > /dev/null 2>&1 || true
	sleep 1
	from=$(count '')
	sleep "$2"
	serial "keiland-shot --socket $dir/keiland-shot.sock --request PING > $dir/ping.txt 2>&1" > /dev/null 2>&1 || true
	sleep 2
	numbers=$(rate "$from")
	timers=$(timers_after "$from")
	found=$(printers_timers "$timers")
	lines=$(echo "$numbers" | sed 's/.* lines=\([0-9]*\)$/\1/')
	if [ -z "$found" ] && [ "$lines" -gt 0 ] && [ "$timers" != "timers fired=?" ]; then
		say "$name: PASS no printers timer fired; $numbers; $timers"
	else
		fail_step "$name" "printers timers '$found'; $numbers; $timers"
	fi
}

# The compositor, in the background, its log in a guest file; no desktop program.
serial "rm -rf $dir; mkdir -p $dir/home/.config/keiland; chmod 700 $dir; XDG_RUNTIME_DIR=$dir HOME=$dir/home /bin/wayland --testing --glass --log-frames --width=1280 --height=800 --timeout=3000 --desktop-client=none > $log 2>&1 < /dev/null &" \
    > "$out/compositor-start.txt" 2>&1 || true

# The start: window mode, the timer thread, the capture listening, the printers prepared with the daemon's program.
if wait_count 'KWL MODE window' 1 120 && wait_count 'KWL THREADS .*timer' 1 30 && wait_count 'KWL SHOT listening' 1 30 &&
    wait_count 'KWL PRINTERS prepared open=1 hosted=1 can=1' 1 10; then
	say "start: PASS window mode, the timer thread, the capture, the printers prepared (hosted, the daemon's program there)"
else
	fail_step start "no window mode, timer thread, capture or printers prepared can=1 (see $out/kwl.log)"
fi
sleep 10

# Idle: two passes in ten seconds at most and no timeout.
idle idle-30 30 2 0

# Settings on its Printers page: a printers object.
program=$(serial 'for p in /bin/settings /usr/bin/settings /bin/keiland-settings-app; do test -x $p && echo "program=$p"; done' 2>/dev/null | tr -d '\r' | grep -E '^program=' | sed -n '1p' | sed 's/^program=//' || true)
objects=$(count 'KWL PRINTERS object')
serial "WAYLAND_DISPLAY=/tmp/wayland-0 XDG_RUNTIME_DIR=$dir HOME=$dir/home $program printers > $dir/settings.log 2>&1 < /dev/null &" > /dev/null 2>&1 || true
if wait_count 'KWL PRINTERS object' $((objects + 1)) 60 && wait_count 'KWL APPS icon app=' 1 60; then
	say "settings: PASS '$program' open on Printers with a printers object"
else
	fail_step settings "no printers object or window for '$program' (see $out/kwl.log)"
fi
sleep 5

# Idle with Settings open: no printers timer.
idle_no_printers idle-settings-30 30

# Add the host's printer: Settings hears the change.
changed=$(settings_count 'PRINTERS changed')
serial "WAYLAND_DISPLAY=/tmp/wayland-0 XDG_RUNTIME_DIR=$dir HOME=$dir/home printtest add lpd 10.0.2.2 $lpd_port lp > $dir/add.txt 2>&1" > /dev/null 2>&1 || true
serial "cat $dir/add.txt" > "$out/add.txt" 2>&1 || true
printer=$(tr -d '\r' < "$out/add.txt" | sed -n "s/^PRINTTEST printer id=\([0-9]*\) .*host=10.0.2.2 port=$lpd_port .*/\1/p" | sed -n 1p)
if grep -q 'PRINTTEST result error=0' "$out/add.txt" && [ -n "$printer" ] && wait_in 'PRINTERS changed' $((changed + 1)) 10 "$dir/settings.log"; then
	say "add: PASS printer $printer added; Settings heard it ($(settings_count 'PRINTERS changed') changes)"
else
	fail_step add "printer '$printer', Settings' changes $(settings_count 'PRINTERS changed') (was $changed; see $out/add.txt)"
fi

# Print a PDF to it: the job ends done; the compositor and Settings hear its states; the host's printer took it.
changed=$(settings_count 'PRINTERS changed')
serial "echo '%PDF-1.4' > $dir/doc.pdf; echo '%%EOF' >> $dir/doc.pdf" > /dev/null 2>&1 || true
serial "WAYLAND_DISPLAY=/tmp/wayland-0 XDG_RUNTIME_DIR=$dir HOME=$dir/home printtest --timeout-s=60 print --printer=$printer $dir/doc.pdf p019 > $dir/print.txt 2>&1; echo status=\$? >> $dir/print.txt" > /dev/null 2>&1 || true
wait_in 'status=' 1 90 "$dir/print.txt" || true
serial "cat $dir/print.txt" > "$out/print.txt" 2>&1 || true
done_lines=$(count "KWL PRINTERS job=[0-9]* printer=$printer state=4")
if grep -q 'status=0' "$out/print.txt" && [ "$done_lines" -ge 1 ] && grep -q 'job queue=lp' "$lpd_log" &&
    [ "$(settings_count 'PRINTERS changed')" -gt $((changed + 1)) ]; then
	say "print: PASS the job done (printtest status 0, $done_lines KWL PRINTERS done lines, the host's printer: $(grep -c 'job queue=lp' "$lpd_log") jobs, Settings' changes $changed -> $(settings_count 'PRINTERS changed'))"
else
	fail_step print "done lines $done_lines, Settings' changes $changed -> $(settings_count 'PRINTERS changed') (see $out/print.txt, $lpd_log)"
fi

# Idle while the daemon still runs: no printers timer.
running=$(serial 'ps -A -o pid,args | grep -c "[k]eiland-printd"' 2>/dev/null | tr -d '\r' | grep -E '^[0-9]+$' | sed -n '$p' || true)
say "printd: ${running:-?} running after the job"
idle_no_printers idle-printd-20 20

# The daemon ends by itself: IDLE after a minute with nothing to do, then BYE.
started=$(date +%s)
gone=0
while [ $(( $(date +%s) - started )) -lt 120 ]; do
	running=$(serial 'ps -A -o pid,args | grep -c "[k]eiland-printd"' 2>/dev/null | tr -d '\r' | grep -E '^[0-9]+$' | sed -n '$p' || true)
	if [ "$running" = "0" ]; then
		gone=1
		break
	fi
	sleep 5
done
if [ "$gone" -eq 1 ]; then
	say "printd-end: PASS the daemon ended by itself $(( $(date +%s) - started )) s after the idle step"
else
	fail_step printd-end "keiland-printd still runs 120 s on"
fi

# The settings file changed behind the compositor's back, as another session would: nothing looks at it by itself.
changed=$(settings_count 'PRINTERS changed')
serial "echo 'printer 50 lpd 10.0.2.50 515 lp Other' >> $dir/home/.config/keiland/printers.conf" > /dev/null 2>&1 || true
sleep 10
quiet=$(settings_count 'PRINTERS changed')
# A new printers object (a client about to show the printers) has the file read; Settings hears the change.
serial "WAYLAND_DISPLAY=/tmp/wayland-0 XDG_RUNTIME_DIR=$dir HOME=$dir/home printtest list > $dir/list1.txt 2>&1" > /dev/null 2>&1 || true
wait_in 'PRINTERS changed' $((changed + 1)) 10 "$dir/settings.log" || true
heard=$(settings_count 'PRINTERS changed')
serial "WAYLAND_DISPLAY=/tmp/wayland-0 XDG_RUNTIME_DIR=$dir HOME=$dir/home printtest list > $dir/list2.txt 2>&1" > /dev/null 2>&1 || true
serial "cat $dir/list2.txt" > "$out/list2.txt" 2>&1 || true
if [ "$quiet" -eq "$changed" ] && [ "$heard" -eq $((changed + 1)) ] && grep -q 'host=10.0.2.50 ' "$out/list2.txt"; then
	say "other-session: PASS not looked at for 10 s (changes $changed -> $quiet), read for a new object (-> $heard), listed after"
else
	fail_step other-session "changes $changed -> $quiet in 10 s -> $heard after a new object (see $out/list2.txt)"
fi

# Remove the host's printer: Settings hears it.
changed=$(settings_count 'PRINTERS changed')
serial "WAYLAND_DISPLAY=/tmp/wayland-0 XDG_RUNTIME_DIR=$dir HOME=$dir/home printtest remove $printer > $dir/remove.txt 2>&1" > /dev/null 2>&1 || true
serial "cat $dir/remove.txt" > "$out/remove.txt" 2>&1 || true
if grep -q 'PRINTTEST result error=0' "$out/remove.txt" && ! grep -q "host=10.0.2.2 port=$lpd_port " "$out/remove.txt" &&
    wait_in 'PRINTERS changed' $((changed + 1)) 10 "$dir/settings.log"; then
	say "remove: PASS printer $printer removed; Settings heard it"
else
	fail_step remove "Settings' changes $changed -> $(settings_count 'PRINTERS changed') (see $out/remove.txt)"
fi

# Settings ended; idle as before.
serial 'for p in $(ps -A -o pid,args | grep "[/]bin/settings" | while read a b; do echo $a; done); do kill $p; done' > /dev/null 2>&1 || true
sleep 3
idle idle-after-settings-30 30 2 0

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

# The guest's logs for the record (read over the serial line: kernel records may be mixed in; not used for the verdict).
serial "grep -e 'KWL PERF [0-9]' -e 'KWL PERF timers' -e 'KWL MODE' -e 'KWL THREADS' -e 'KWL TIMER' -e 'KWL PRINTERS' -e 'KWL EXIT' -e 'KWL FAILED' $log" > "$out/kwl.log" 2>&1 || true
serial "grep -e PRINTERS -e FAILED $dir/settings.log" > "$out/settings.log" 2>&1 || true

if [ "$failed" -eq 0 ]; then
	say "qemu-printers: PASS"
else
	say "qemu-printers: FAIL"
	exit 1
fi

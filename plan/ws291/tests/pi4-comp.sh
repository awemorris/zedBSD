#!/bin/sh
# ws291-p025: the Raspberry Pi 4's desktop frames (frame-ops) with a compositor of the tree, without a reboot, run from the
# top of the tree on a board already running the kernel of plan/ws291/tests/config-rpi4-desk.mk (pi4-desk.sh put it in)
# with the console login:
#   [PI=ADDRESS] [WAYLAND=BIN] [TIMING=1] [SETTLE=SECONDS] [RUNS=1] [OPTIONS='...'] sh plan/ws291/tests/pi4-comp.sh OUT
# Each run stops the compositor running (and its clients), starts WAYLAND (copied to /root/p025desk/wayland; the board's
# /bin/wayland without it) as root on a new socket with the session's options (--glass, the wallpaper, --log-frames,
# OPTIONS), VULKAN_ZEDBSD_SUBMIT_TIMING=1 with TIMING=1, waits SETTLE seconds (default 40) of a still desktop and runs
# plan/tools/frame-ops/frame-ops.sh with the Terminal started by the shell (TERMINAL_PERF=1; OUT/runN: terminal.log, session.log, marks, table.txt and frames.txt of frame-times.py, shot.png
# of keiland-shot at the end, SHOT or build/p025-4d/bin/keiland-shot).  The board's files are not changed.  OUT/summary.txt holds what is printed.
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
set -u
out=${1:?usage: pi4-comp.sh OUT}
PI=${PI:-10.0.30.6}
KEY=plan/tmp/guest/id_ed25519
SO="-i $KEY -o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null -o LogLevel=ERROR -o ConnectTimeout=8 -o BatchMode=yes"
D=/root/p025desk
mkdir -p "$out"
summary=$out/summary.txt
: > "$summary"

say() {
	echo "$*" | tee -a "$summary"
}

b() {
	timeout "${BT:-60}" ssh $SO "root@$PI" "$@"
}

say "start $(date +%H:%M:%S) wayland ${WAYLAND:-board} timing ${TIMING:-0} options ${OPTIONS:-}"
bin=/bin/wayland
if [ -n "${WAYLAND:-}" ]; then
	bin=$D/wayland
	b "mkdir -p $D"
	timeout 120 scp $SO -q "$WAYLAND" "root@$PI:$D/wayland.new" &&
	    b "test \"\$(cksum < $D/wayland.new | cut -d' ' -f1)\" = $(cksum < "$WAYLAND" | cut -d' ' -f1) && chmod 755 $D/wayland.new && mv $D/wayland.new $D/wayland" ||
	    { say "FAIL the compositor was not put in"; exit 1; }
fi
timeout 120 scp $SO -q "${SHOT:-build/p025-4d/bin/keiland-shot}" "root@$PI:$D/keiland-shot" || say "FAIL keiland-shot was not put in"
# Settings' Welcome is not started (root's welcome.done=1; the file is kept once as desktop.conf.p025old): its spawn
# stalls the compositor until the kernel's fork is fixed (BUG-379).
b 'c=/root/.config/keiland/desktop.conf; test -e $c.p025old || cp $c $c.p025old; grep -q "^welcome.done=1" $c || echo welcome.done=1 >> $c'
env=""
[ "${TIMING:-0}" = 1 ] && env="VULKAN_ZEDBSD_SUBMIT_TIMING=1"

# A graphical login's compositor (the greeter's or a session's) would take the display back: the board must have booted
# with the console login (pi4-desk.sh leaves login=console for the next boot, so reboot after it).
if b 'ps -A -o user,comm' | awk '$2 ~ /wayland$/ && $1 != "root"' | grep -q .; then
	say "FAIL a graphical login runs a compositor; reboot the board with login=console first"
	exit 1
fi

i=1
while [ $i -le "${RUNS:-1}" ]; do
	d=$out/run$i
	mkdir -p "$d"
	# The compositor running and its clients stop; a new socket's directory each run.
	b 'for p in $(ps -A -o pid,comm | awk '"'"'$2 ~ /(wayland|files|keiland-ime|terminal|settings)$/ {print $1}'"'"'); do kill $p; done; sleep 3; for p in $(ps -A -o pid,comm | awk '"'"'$2 ~ /(wayland|files|keiland-ime|terminal|settings)$/ {print $1}'"'"'); do kill -9 $p; done' > /dev/null 2>&1
	sleep 2
	BT=120 b 'sysctl hw.gpu.start=1' > /dev/null 2>&1
	run=$D/run$(date +%H%M%S)
	log=$D/session-$i.log
	b "mkdir -p $run; chmod 700 $run; cd $D; XDG_RUNTIME_DIR=$run HOME=/root $env nohup $bin --glass --log-frames --wallpaper=/usr/share/keiland/wallpaper.png --socket=$run/wayland-0 ${OPTIONS:-} > $log 2>&1 < /dev/null &" > /dev/null 2>&1
	n=0
	while [ $n -lt 30 ] && ! b "grep -q 'KWL LAT draw' $log" 2> /dev/null; do
		sleep 3
		n=$((n + 1))
	done
	say "run$i $bin cksum $(b "cksum < $bin" | cut -d' ' -f1) log $(b "wc -l < $log" 2>&1)"
	sleep "${SETTLE:-40}"
	# The launch step closes App Home (Super) instead of starting the Terminal, which the shell starts (the compositor's spawn
	# stalls until BUG-379 is fixed); frame-ops then finds the Terminal's KWL MAP as before.
	sed '0,/^input key enter$/s//input key super; sh -c "$LAUNCH" \&/' plan/tools/frame-ops/frame-ops.sh > "$d/frame-ops.sh"
	timeout 120 scp $SO -q "$d/frame-ops.sh" "root@$PI:/tmp/frame-ops.sh"
	BT=400 b "LAUNCH='XDG_RUNTIME_DIR=$run WAYLAND_DISPLAY=wayland-0 HOME=/root TERMINAL_PERF=1 $env exec /bin/terminal > /tmp/terminal.log 2>&1 < /dev/null' sh /tmp/frame-ops.sh $log /tmp/marks 1920 1080 > /tmp/frame-ops.out 2>&1 < /dev/null" > /dev/null 2>&1
	BT=90 b "cd $D; timeout 60 $D/keiland-shot --socket $run/keiland-shot.sock /tmp/p025-shot.png > /tmp/shot.out 2>&1" > /dev/null 2>&1
	timeout 120 scp $SO -q "root@$PI:$log" "$d/session.log"
	timeout 120 scp $SO -q "root@$PI:/tmp/marks" "root@$PI:/tmp/frame-ops.out" "root@$PI:/tmp/p025-shot.png" "root@$PI:/tmp/terminal.log" "$d/" || say "run$i FAIL not all collected"
	b 'dmesg' > "$d/dmesg.txt" 2>&1
	python3 -I plan/tools/frame-ops/frame-times.py "$d/session.log" "$d/marks" > "$d/table.txt" 2>&1
	python3 -I plan/tools/frame-ops/frame-times.py "$d/session.log" "$d/marks" --frames > "$d/frames.txt" 2>&1
	sed "s/^/run$i /" "$d/table.txt" | tee -a "$summary"
	say "run$i terminal $(grep -c 'KWL MAP client=' "$d/session.log") maps, home reads $(grep -c 'KWL HOME read' "$d/session.log"), $(cat "$d/frame-ops.out" | tr '\n' ' ')"
	i=$((i + 1))
done
say "end $(date +%H:%M:%S)"

#!/bin/sh
# The input devices' nodes stay root's while sessiond runs the session and the greeter, on the QEMU Venus guest:
# builds the image (plan/ws353/tests/config-amd64-seat.mk: the graphical login, whose automatic login starts the
# image's user's session at boot), boots it, and judges by the guest's answers over SSH (ls -ln of /dev/input,
# sessiond's /var/log/sessiond.log, the compositors' logs, the capture's POINTER) and by QMP; the console is the
# login screen's and is never read.
#
# Steps: the session is up and its compositor opened the devices of the boot through sessiond; every eventN of
# /dev/input is root's and wheel's, 0640, and sessiond's log never gave one away; a USB mouse plugged with QMP is
# taken through one OPEN, its node is root's too, and its motion moves the session's pointer (the capture's POINTER
# before and after); unplugged; the session's compositor ended (SIGTERM): the greeter comes and opens event0 through
# its own seat socket; a USB keyboard plugged in the greeter is taken through an OPEN of the greeter's, and the
# image's user's PIN (0000, which the login screen asks first) typed on it and Enter log in: sessiond starts the user's session again, whose
# compositor says READY; the nodes are still root's; 20 s idle of the session with no timeout.
#
#   sh plan/ws353/tests/qemu-seat-owner.sh [OUT]
#
# OUT (default build/ws353-p020-qemu) gets summary.txt, the make logs and the guest's log lines.  The guest's runtime
# is build/ws353-p020-run.  IMAGE=<file> skips the build (build/x86_64/hdd-image.img otherwise).
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
set -eu
cd "$(dirname -- "$0")/../../.."
out=${1:-build/ws353-p020-qemu}
config=plan/ws353/tests/config-amd64-seat.mk
export GUEST_RUNTIME=$PWD/build/ws353-p020-run
# The host's Venus renderer (plan/ws353/tests/build-venus-renderer.sh makes it), on Lavapipe with zink for QEMU's GL.
VENUS_RENDERER=${VENUS_RENDERER:-$PWD/build/ws035-sq-venus/install}
export LIBGL_ALWAYS_SOFTWARE=1
export MESA_LOADER_DRIVER_OVERRIDE=zink
export VK_DRIVER_FILES="${VK_DRIVER_FILES:-/usr/share/vulkan/icd.d/lvp_icd.json}"
export RENDER_SERVER_EXEC_PATH="${RENDER_SERVER_EXEC_PATH:-$VENUS_RENDERER/libexec/virgl_render_server}"
export LD_LIBRARY_PATH="$VENUS_RENDERER/lib/x86_64-linux-gnu${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
if [ ! -x "$RENDER_SERVER_EXEC_PATH" ]; then
	echo "qemu-seat-owner: FAIL no Venus renderer at $RENDER_SERVER_EXEC_PATH (plan/ws353/tests/build-venus-renderer.sh)" >&2
	exit 1
fi
. plan/tools/guest/venus-hostmem.sh
. plan/tools/fresh-out.sh
fresh_out "$out"
summary=$out/summary.txt
: > "$summary"
guest="python3 plan/tools/guest/guest.py"
session=/run/user/1000/session.log
sessiond=/var/log/sessiond.log
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
	timeout 5400 flock /tmp/zedbsd-image-build.lock make -j4 BUILD_JOBS=4 CONFIGMK=$config userland > "$out/make-userland.txt" 2>&1
	timeout 3000 flock /tmp/zedbsd-image-build.lock make -j4 BUILD_JOBS=4 CONFIGMK=$config disk > "$out/make-disk.txt" 2>&1
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
    > "$out/start.txt" 2>&1 || true

# One command over SSH.
run() {
	timeout 60 $guest run "$1" 2>/dev/null </dev/null
}
tries=0
until run 'uname -a' > "$out/uname.txt" 2>&1; do
	tries=$((tries + 1))
	if [ "$tries" -ge 24 ]; then
		say "qemu-seat-owner: FAIL no SSH (see $out/start.txt)"
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

# How many lines of a guest file match (-1 when the guest did not answer).
count_in() {
	tries=0
	while [ "$tries" -lt 4 ]; do
		number=$(run "grep -c -e '$1' $2" | tr -d '\r' | grep -E '^[0-9]+$' | sed -n '$p' || true)
		if [ -n "$number" ]; then
			echo "$number"
			return 0
		fi
		tries=$((tries + 1))
		sleep 1
	done
	echo -1
}

# Waits until at least MIN lines of a guest file match, at most LIMIT seconds; the check is the command's status.
wait_in() {
	started=$(date +%s)
	while ! run "test \$(grep -c -e '$1' $4) -ge $2" >/dev/null 2>&1; do
		if [ $(( $(date +%s) - started )) -ge "$3" ]; then
			return 1
		fi
		sleep 1
	done
}

# The KWL PERF windows of the session's log after line FROM, as "ms=M passes=P timeouts=T lines=N".
rate() {
	line=$(run "sh /root/perf-rate.sh $1 $session" | tr -d '\r' | grep -E '^ms=[0-9]+ passes=[0-9]+ timeouts=[0-9]+ lines=[0-9]+$' | sed -n '$p' || true)
	if [ -z "$line" ]; then
		line="ms=0 passes=0 timeouts=0 lines=0"
	fi
	echo "$line"
}

# Asks the session's capture for a PING: a pass that prints the KWL PERF window under way when it is five seconds old.
ping() {
	run "keiland-shot --socket /run/user/1000/keiland-shot.sock --request PING > /tmp/p20-ping.txt 2>&1" > /dev/null 2>&1 || true
}

# An idle stretch of SECONDS: its KWL PERF windows (the window under way is ended first by a PING five seconds after
# it began, the stretch's by the PING at its end), kept in the variable named by the first argument.
idle() {
	sleep 6
	ping
	sleep 1
	from=$(count_in '' "$session")
	sleep "$2"
	ping
	sleep 2
	numbers=$(rate "$from")
	say "$1: $numbers"
	eval "$1_numbers=\"\$numbers\""
}


# Sends QMP input events in one connection (QEMU gives them to the input device activated last, the one just
# plugged): each argument is a qcode to press and release, or rel:DX:DY for a relative motion.
qmp_input() {
	timeout 60 python3 -I -c '
import json, socket, sys, time
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
failed = 0
ask({"execute": "qmp_capabilities"})
for item in sys.argv[2:]:
	if item.startswith("rel:"):
		dx, dy = item[4:].split(":")
		groups = [[{"type": "rel", "data": {"axis": "x", "value": int(dx)}}, {"type": "rel", "data": {"axis": "y", "value": int(dy)}}]]
	else:
		key = {"type": "qcode", "data": item}
		groups = [[{"type": "key", "data": {"down": True, "key": key}}], [{"type": "key", "data": {"down": False, "key": key}}]]
	for events in groups:
		answer = ask({"execute": "input-send-event", "arguments": {"events": events}})
		print(json.dumps(answer))
		if "error" in answer:
			failed = 1
		time.sleep(0.08)
sys.exit(failed)
' "$GUEST_RUNTIME/qmp.sock" "$@" >> "$out/qmp.txt" 2>&1
}

# The eventN nodes of /dev/input not root's and wheel's with crw-r----- (none when all are), or "none-listed".
nodes_not_root() {
	listing=$(run 'ls -ln /dev/input' | tr -d '\r')
	echo "$listing" >> "$out/ls-input.txt"
	if ! echo "$listing" | grep -q ' event[0-9]*$'; then
		echo none-listed
		return 0
	fi
	echo "$listing" | grep ' event[0-9]*$' | awk '$1 != "crw-r-----" || $3 != "0" || $4 != "0" { print $NF }' | tr '\n' ' '
}

# The session's pointer as the capture reports it (x=X y=Y), empty when it did not answer.
pointer() {
	run "keiland-shot --socket /run/user/1000/keiland-shot.sock --request POINTER" | tr -d '\r' | sed -n 's/.*\(x=-*[0-9]* y=-*[0-9]*\).*/\1/p' | sed -n '$p'
}

# 1. The session up, its devices of the boot opened through sessiond.
if wait_in 'KWL READY.*role=normal' 1 240 "$session" && wait_in 'KWL DEVICES started' 1 30 "$session"; then
	say "session: PASS the session's compositor is up and its devices' thread started"
else
	fail_step session "no KWL READY role=normal or KWL DEVICES started in $session"
fi
boot_opens=$(count_in 'KL SEAT open node=event[0-9]* error=0' "$session")
if [ "$boot_opens" -ge 1 ]; then
	say "boot-devices: PASS ${boot_opens} devices of the boot opened through sessiond"
else
	fail_step boot-devices "no device opened through sessiond ($boot_opens)"
fi

# 2. The nodes are root's, and sessiond gave none away.
others=$(nodes_not_root)
if [ -z "$others" ]; then
	say "nodes-session: PASS every eventN of /dev/input is root:wheel crw-r----- in the session"
else
	fail_step nodes-session "not root's: $others"
fi
given=$(count_in 'SESSIOND SEAT path=/dev/input/event' "$sessiond")
if [ "$given" -eq 0 ]; then
	say "no-chown: PASS sessiond's log gives no eventN node away"
else
	fail_step no-chown "$given lines give an eventN node away"
fi
sleep 5

# 3. A USB mouse plugged in: taken through one OPEN, its node root's, and its motion moves the pointer.
taken=$(count_in 'KWL INPUT device=.*kind=pointer' "$session")
asked=$(count_in 'SESSIOND SEAT open node=' "$sessiond")
if qmp device_add '{"driver": "usb-mouse", "bus": "xhci.0", "id": "p20mouse"}' && wait_in 'KWL INPUT device=.*kind=pointer' $((taken + 1)) 20 "$session"; then
	node=$(run "grep 'KWL INPUT device=' $session" | tr -d '\r' | grep 'kind=pointer' | sed -n '$p' | sed -n 's|.*device=/dev/input/\(event[0-9]*\) .*|\1|p')
	requests=$(( $(count_in 'SESSIOND SEAT open node=' "$sessiond") - asked ))
	last=$(run "grep 'SESSIOND SEAT open node=' $sessiond" | tr -d '\r' | sed -n '$p')
	if [ -n "$node" ] && [ "$requests" -eq 1 ] && echo "$last" | grep -q "node=$node error=0 holder=1"; then
		say "mouse-taken: PASS $node taken through one OPEN of the session's"
	else
		fail_step mouse-taken "node=${node:-none} requests=$requests last=$last"
	fi
else
	fail_step mouse-taken "not taken within 20 s"
fi
others=$(nodes_not_root)
if [ -z "$others" ]; then
	say "nodes-plugged: PASS the plugged mouse's node is root's too"
else
	fail_step nodes-plugged "not root's: $others"
fi
before=$(pointer)
qmp_input rel:-40:-30 rel:-40:-30 rel:-40:-30 || true
sleep 2
after=$(pointer)
if [ -n "$before" ] && [ -n "$after" ] && [ "$before" != "$after" ]; then
	say "mouse-moves: PASS the pointer went from $before to $after"
else
	fail_step mouse-moves "pointer before '${before}' after '${after}'"
fi
closed=$(count_in 'KWL INPUT_CLOSED device=' "$session")
if qmp device_del '{"id": "p20mouse"}' && wait_in 'KWL INPUT_CLOSED device=' $((closed + 1)) 20 "$session"; then
	say "mouse-closed: PASS closed after the unplug"
else
	fail_step mouse-closed "not closed within 20 s"
fi

# 4. Idle of the session: no timeout.
sleep 5
idle idle_session 20
idle_ms=$(echo "$idle_session_numbers" | sed 's/^ms=\([0-9]*\) .*/\1/')
idle_timeouts=$(echo "$idle_session_numbers" | sed 's/.* timeouts=\([0-9]*\) .*/\1/')
if [ "$idle_ms" -gt 0 ] && [ "$idle_timeouts" -eq 0 ]; then
	say "idle: PASS no timeout of the session's compositor while idle ($idle_session_numbers)"
else
	fail_step idle "$idle_session_numbers"
fi
run "grep -e 'SESSIOND SEAT' -e 'SESSIOND SESSION' -e 'SESSIOND HANDOFF' $sessiond" > "$out/sessiond-1.txt" 2>&1 || true
run "grep -e 'KWL READY' -e 'KWL DEVICES' -e 'KL DEVICES' -e 'KL SEAT' -e 'KWL INPUT' -e 'KWL PERF [0-9]' -e 'KWL FAILED' $session" > "$out/session-1.txt" 2>&1 || true

# 5. The session's compositor ended: the greeter comes and opens event0 through its own seat socket.
greeters=$(count_in 'KWL DEVICES started' /var/log/greeter.log)
[ "$greeters" -ge 0 ] || greeters=0
granted0=$(count_in 'SESSIOND SEAT open node=event0 error=0 holder=1' "$sessiond")
run 'for p in $(ps -A -o pid,args | grep "[/]bin/wayland --session" | while read a b; do echo $a; done); do kill $p; done' \
    > "$out/kill.txt" 2>&1 || true
if wait_in 'KWL DEVICES started' $((greeters + 1)) 120 /var/log/greeter.log && wait_in 'SESSIOND SEAT open node=event0 error=0 holder=1' $((granted0 + 1)) 60 "$sessiond"; then
	say "greeter: PASS the greeter came and opened event0 through its own seat socket"
else
	fail_step greeter "no devices' thread in the greeter, or no OPEN of event0 granted to it"
fi
others=$(nodes_not_root)
if [ -z "$others" ]; then
	say "nodes-greeter: PASS every eventN is root's under the greeter"
else
	fail_step nodes-greeter "not root's: $others"
fi
sleep 5

# 6. A USB keyboard plugged in the greeter: taken through an OPEN of the greeter's; the user's PIN typed on it logs in
#    (the image's account has the PIN 0000, which the login screen asks first).
taken=$(count_in 'KWL INPUT device=.*kind=keyboard' /var/log/greeter.log)
starts=$(count_in 'SESSIOND SESSION start user=user' "$sessiond")
if qmp device_add '{"driver": "usb-kbd", "bus": "xhci.0", "id": "p20kbd"}' && wait_in 'KWL INPUT device=.*kind=keyboard' $((taken + 1)) 20 /var/log/greeter.log; then
	knode=$(run "grep 'KWL INPUT device=' /var/log/greeter.log" | tr -d '\r' | grep 'kind=keyboard' | sed -n '$p' | sed -n 's|.*device=/dev/input/\(event[0-9]*\) .*|\1|p')
	last=$(run "grep 'SESSIOND SEAT open node=' $sessiond" | tr -d '\r' | sed -n '$p')
	if [ -n "$knode" ] && echo "$last" | grep -q "node=$knode error=0 holder=1"; then
		say "keyboard-taken: PASS $knode taken by the greeter through its OPEN"
	else
		fail_step keyboard-taken "node=${knode:-none} last=$last"
	fi
else
	fail_step keyboard-taken "not taken by the greeter within 20 s"
fi
sleep 2
qmp_input 0 0 0 0 ret || true
if wait_in 'SESSIOND SESSION start user=user' $((starts + 1)) 60 "$sessiond" && wait_in 'KWL READY.*role=normal' 1 120 "$session"; then
	say "login: PASS the PIN typed on the plugged keyboard logged the user in, and the session's compositor is up"
else
	fail_step login "no new session of user (or no READY) within the time"
fi
others=$(nodes_not_root)
if [ -z "$others" ]; then
	say "nodes-login: PASS every eventN is root's in the new session"
else
	fail_step nodes-login "not root's: $others"
fi
given=$(count_in 'SESSIOND SEAT path=/dev/input/event' "$sessiond")
if [ "$given" -eq 0 ]; then
	say "no-chown-end: PASS sessiond gave no eventN node away in the whole run"
else
	fail_step no-chown-end "$given lines give an eventN node away"
fi

# The logs for the record (not used for the verdict).
run "grep -e 'SESSIOND SEAT' -e 'SESSIOND SESSION' -e 'SESSIOND HANDOFF' -e 'SESSIOND GREETER' -e 'SESSIOND AUTH' $sessiond" > "$out/sessiond.txt" 2>&1 || true
run "grep -e 'KWL READY' -e 'KWL DEVICES' -e 'KL DEVICES' -e 'KL SEAT' -e 'KWL INPUT' -e 'KWL FAILED' -e 'KWL GREETER' -e 'KWL SLEEP' -e 'KWL LOCK' /var/log/greeter.log" > "$out/greeter.txt" 2>&1 || true
run "grep -e 'KWL READY' -e 'KWL DEVICES' -e 'KL DEVICES' -e 'KL SEAT' -e 'KWL INPUT' -e 'KWL FAILED' $session" > "$out/session-2.txt" 2>&1 || true
run 'ls -ln /dev/input /dev/gpu0' > "$out/ls-end.txt" 2>&1 || true

if [ "$failed" -eq 0 ]; then
	say "qemu-seat-owner: PASS"
else
	say "qemu-seat-owner: FAIL"
	exit 1
fi

#!/bin/sh
# Input devices plugged in after the login, opened for the session's compositor by sessiond, on the QEMU Venus guest:
# builds the image (plan/ws353/tests/config-amd64-seat.mk: the graphical login, whose automatic login starts the
# image's user's session at boot), boots it, and judges by counting lines of the guest's logs over SSH (sessiond's
# /var/log/sessiond.log, the session's /run/user/1000/session.log; the console is the login screen's and is never
# read).
#
# Steps: the session is up with its seat socket (KWL DEVICES started, sessiond's SEAT holder, the devices of the boot
# opened through sessiond: KL SEAT open ... error=0); 20 s idle (the KWL PERF windows); a USB keyboard plugged with QMP:
# the compositor takes it (KWL INPUT ... kind=keyboard) after exactly one OPEN of its node in sessiond's log
# (granted), the devices' thread opened it (KL DEVICES input came ... error=0), and no scan followed (no new
# KWL EVENT input changed: the settle scan is gone); unplugged: closed; the same with a USB mouse (kind=pointer);
# 20 s idle again (no more passes than before the plugs, give or take one in ten seconds); sessiond's log has no
# refused OPEN of the session (holder=0) and no lost answer.
#
#   sh plan/ws353/tests/qemu-seat-hotplug.sh [OUT]
#
# OUT (default build/ws353-p018-qemu) gets summary.txt, the make logs and the guest's log lines.  The guest's runtime
# is build/ws353-p018-run.  IMAGE=<file> skips the build (build/x86_64/hdd-image.img otherwise).
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
set -eu
cd "$(dirname -- "$0")/../../.."
out=${1:-build/ws353-p018-qemu}
config=plan/ws353/tests/config-amd64-seat.mk
export GUEST_RUNTIME=$PWD/build/ws353-p018-run
# The host's Venus renderer (plan/ws353/tests/build-venus-renderer.sh makes it), on Lavapipe with zink for QEMU's GL.
VENUS_RENDERER=${VENUS_RENDERER:-$PWD/build/ws035-sq-venus/install}
export LIBGL_ALWAYS_SOFTWARE=1
export MESA_LOADER_DRIVER_OVERRIDE=zink
export VK_DRIVER_FILES="${VK_DRIVER_FILES:-/usr/share/vulkan/icd.d/lvp_icd.json}"
export RENDER_SERVER_EXEC_PATH="${RENDER_SERVER_EXEC_PATH:-$VENUS_RENDERER/libexec/virgl_render_server}"
export LD_LIBRARY_PATH="$VENUS_RENDERER/lib/x86_64-linux-gnu${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
if [ ! -x "$RENDER_SERVER_EXEC_PATH" ]; then
	echo "qemu-seat-hotplug: FAIL no Venus renderer at $RENDER_SERVER_EXEC_PATH (plan/ws353/tests/build-venus-renderer.sh)" >&2
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
		say "qemu-seat-hotplug: FAIL no SSH (see $out/start.txt)"
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
	run "keiland-shot --socket /run/user/1000/keiland-shot.sock --request PING > /tmp/p18-ping.txt 2>&1" > /dev/null 2>&1 || true
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

# 1. The session up with its seat socket.
if wait_in 'KWL READY.*role=normal' 1 240 "$session" && wait_in 'KWL DEVICES started' 1 30 "$session"; then
	say "session: PASS the session's compositor is up and its devices' thread started"
else
	fail_step session "no KWL READY role=normal or KWL DEVICES started in $session"
fi
if wait_in 'SESSIOND SEAT holder' 1 10 "$sessiond"; then
	say "holder: PASS sessiond gave the seat socket the input devices"
else
	fail_step holder "no SESSIOND SEAT holder in $sessiond"
fi
boot_opens=$(count_in 'KL SEAT open node=event[0-9]* error=0' "$session")
if [ "$boot_opens" -ge 1 ]; then
	say "boot-devices: PASS ${boot_opens} devices of the boot opened through sessiond"
else
	fail_step boot-devices "no device opened through sessiond ($boot_opens)"
fi
sleep 10

# 2. Idle before the plugs.
idle idle_before 20

# 3. Plugs one device and checks it came through one OPEN, and pulls it out: plug DRIVER ID KIND
plug() {
	taken=$(count_in "KWL INPUT device=.*kind=$3" "$session")
	scans=$(count_in 'KWL EVENT input changed' "$session")
	opens=$(count_in 'SESSIOND SEAT open node=event[0-9]* error=0 holder=1' "$sessiond")
	asked=$(count_in 'SESSIOND SEAT open node=' "$sessiond")
	came=$(count_in 'KL DEVICES input came node=event[0-9]* error=0' "$session")
	plugged_at=$(now_ms)
	if qmp device_add "{\"driver\": \"$1\", \"bus\": \"xhci.0\", \"id\": \"$2\"}" && wait_in "KWL INPUT device=.*kind=$3" $((taken + 1)) 20 "$session"; then
		say "$2-taken: PASS taken within $(( $(now_ms) - plugged_at )) ms of the plug (to the SSH check's resolution)"
	else
		fail_step "$2-taken" "not taken within 20 s"
		return
	fi
	node=$(run "grep 'KWL INPUT device=' $session" | tr -d '\r' | grep "kind=$3" | sed -n '$p' | sed -n 's|.*device=/dev/input/\(event[0-9]*\) .*|\1|p')
	granted=$(( $(count_in 'SESSIOND SEAT open node=event[0-9]* error=0 holder=1' "$sessiond") - opens ))
	requests=$(( $(count_in 'SESSIOND SEAT open node=' "$sessiond") - asked ))
	last=$(run "grep 'SESSIOND SEAT open node=' $sessiond" | tr -d '\r' | sed -n '$p')
	opened=$(( $(count_in 'KL DEVICES input came node=event[0-9]* error=0' "$session") - came ))
	if [ -n "$node" ] && [ "$granted" -eq 1 ] && [ "$requests" -eq 1 ] && [ "$opened" -eq 1 ] && echo "$last" | grep -q "node=$node error=0 holder=1"; then
		say "$2-one-open: PASS $node opened by sessiond on one request, from the devices' thread"
	else
		fail_step "$2-one-open" "node=${node:-none} requests=$requests granted=$granted devices' thread=$opened last=$last"
	fi
	sleep 3
	after=$(count_in 'KWL EVENT input changed' "$session")
	if [ "$after" -eq "$scans" ]; then
		say "$2-no-scan: PASS no scan after the plug"
	else
		fail_step "$2-no-scan" "$((after - scans)) scans asked after the plug"
	fi
	closed=$(count_in "KWL INPUT_CLOSED device=/dev/input/$node" "$session")
	if qmp device_del "{\"id\": \"$2\"}" && wait_in "KWL INPUT_CLOSED device=/dev/input/$node" $((closed + 1)) 20 "$session"; then
		say "$2-closed: PASS closed after the unplug"
	else
		fail_step "$2-closed" "not closed within 20 s"
	fi
}
plug usb-kbd p18kbd keyboard
plug usb-mouse p18mouse pointer
sleep 5

# 4. Idle after the plugs: no pace left behind.
idle idle_after 20
before_ms=$(echo "$idle_before_numbers" | sed 's/^ms=\([0-9]*\) .*/\1/')
before_passes=$(echo "$idle_before_numbers" | sed 's/.* passes=\([0-9]*\) .*/\1/')
after_ms=$(echo "$idle_after_numbers" | sed 's/^ms=\([0-9]*\) .*/\1/')
after_passes=$(echo "$idle_after_numbers" | sed 's/.* passes=\([0-9]*\) .*/\1/')
if [ "$before_ms" -gt 0 ] && [ "$after_ms" -gt 0 ] && [ $(( after_passes * 10000 * before_ms )) -le $(( (before_passes * 10000 + before_ms) * after_ms )) ]; then
	say "idle: PASS the passes after the plugs (${after_passes} in ${after_ms} ms) are no more than before (${before_passes} in ${before_ms} ms) and one in ten seconds"
else
	fail_step idle "before: $idle_before_numbers, after: $idle_after_numbers"
fi

# 5. sessiond refused nothing of the session and lost no answer.
refused=$(count_in 'SESSIOND SEAT open .*holder=0' "$sessiond")
lost=$(count_in 'SESSIOND SEAT answer lost' "$sessiond")
if [ "$refused" -eq 0 ] && [ "$lost" -eq 0 ]; then
	say "sessiond: PASS no refused OPEN, no lost answer"
else
	fail_step sessiond "refused=$refused lost=$lost"
fi

# The logs for the record (not used for the verdict).
run "grep -e 'SESSIOND SEAT' -e 'SESSIOND SESSION' $sessiond" > "$out/sessiond.txt" 2>&1 || true
run "grep -e 'KWL READY' -e 'KWL DEVICES' -e 'KL DEVICES' -e 'KL SEAT' -e 'KL EVENTS' -e 'KWL INPUT' -e 'KWL EVENT' -e 'KWL PERF [0-9]' -e 'KWL FAILED' $session" > "$out/session.txt" 2>&1 || true

if [ "$failed" -eq 0 ]; then
	say "qemu-seat-hotplug: PASS"
else
	say "qemu-seat-hotplug: FAIL"
	exit 1
fi

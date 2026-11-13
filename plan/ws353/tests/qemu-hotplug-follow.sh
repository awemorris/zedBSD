#!/bin/sh
# The compositor's follow of the displays on the QEMU Venus guest: builds the image
# (plan/ws353/tests/config-amd64-follow.mk), boots it with two Venus heads on QEMU's D-Bus display, starts the
# compositor from the serial line with its log in a guest file, plugs and unplugs the heads with
# plan/tools/guest/venus-head.sh, and judges by counting the compositor's KWL lines in that file with commands on the
# serial line (the console's text is never read for the verdict).
#
# Steps: the start (the hotplug thread in KWL THREADS, its first fence, two displays read, window mode); 60 s with no
# change (no hotplug told and no reading of the displays); head 1 unplugged and plugged (the head follows); head 0, the
# display shown, unplugged (the output moves to display 1) and plugged (it stays there, display 0 a head); 30 s with
# no change again; SIGTERM (the hotplug thread stopped and joined: its wakes are the hotplugs told and the stop, the
# compositor ends within 20 s); no KWL FAILED.  QEMU's plug can take about 1 s to reach the guest, so the steps wait
# up to 30 s for their lines and judge by counts, not by time.
#
#   sh plan/ws353/tests/qemu-hotplug-follow.sh [OUT]
#
# OUT (default build/ws353-p004-qemu) gets summary.txt, the make logs, the guest's log and the host's timings.  The
# guest's runtime is build/ws353-p004-run.  The image is build/x86_64/hdd-image.img (the build keeps the x86_64
# sysroot in build/x86_64 whatever BUILD says).  IMAGE=<file> skips the build.
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
set -eu
cd "$(dirname -- "$0")/../../.."
out=${1:-build/ws353-p004-qemu}
config=plan/ws353/tests/config-amd64-follow.mk
export GUEST_RUNTIME=$PWD/build/ws353-p004-run
# The host's Venus renderer (plan/ws353/tests/build-venus-renderer.sh makes it), on Lavapipe with zink for QEMU's GL.
VENUS_RENDERER=${VENUS_RENDERER:-$PWD/build/ws035-sq-venus/install}
export LIBGL_ALWAYS_SOFTWARE=1
export MESA_LOADER_DRIVER_OVERRIDE=zink
export VK_DRIVER_FILES="${VK_DRIVER_FILES:-/usr/share/vulkan/icd.d/lvp_icd.json}"
export RENDER_SERVER_EXEC_PATH="${RENDER_SERVER_EXEC_PATH:-$VENUS_RENDERER/libexec/virgl_render_server}"
export LD_LIBRARY_PATH="$VENUS_RENDERER/lib/x86_64-linux-gnu${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
if [ ! -x "$RENDER_SERVER_EXEC_PATH" ]; then
	echo "qemu-hotplug-follow: FAIL no Venus renderer at $RENDER_SERVER_EXEC_PATH (plan/ws353/tests/build-venus-renderer.sh)" >&2
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
    "-object memory-backend-memfd,id=mem,size=8G,share=on -machine memory-backend=mem -device virtio-gpu-gl-pci,id=venus,venus=on,blob=on,hostmem=$VENUS_HOSTMEM,max_outputs=2 -display dbus,gl=on,rendernode=/dev/dri/renderD128" \
    > "$out/start.txt"

# One short command on the serial line (it logs in first when needed); a garbled status costs 20 s, not 120.
serial() {
	timeout 30 python3 plan/tools/guest/serial.py --socket "$GUEST_RUNTIME/serial.sock" --timeout 20 run "$1"
}
tries=0
until serial 'uname -a' > "$out/uname.txt" 2>&1; do
	tries=$((tries + 1))
	if [ "$tries" -ge 12 ]; then
		say "qemu-hotplug-follow: FAIL no shell on the serial line (see $out/uname.txt)"
		exit 1
	fi
	sleep 10
done
head_set() {
	timeout 30 sh plan/tools/guest/venus-head.sh "$1" "$2" >> "$out/heads.txt" 2>&1
}
now_ms() {
	date +%s%3N
}

# How many lines of the compositor's log match: the last all-digit line of grep -c (a kernel record may land in the
# serial output), tried again when none came.
log=/tmp/p4/kwl.log
count() {
	tries=0
	while [ "$tries" -lt 4 ]; do
		number=$(serial "grep -c '$1' $log" 2>/dev/null | tr -d '\r' | grep -E '^[0-9]+$' | sed -n '$p' || true)
		if [ -n "$number" ]; then
			echo "$number"
			return 0
		fi
		tries=$((tries + 1))
		sleep 1
	done
	echo -1
}

# Waits until at least MIN lines match, at most LIMIT seconds; the check is the command's status, tried again when
# the line is garbled.
wait_count() {
	started=$(date +%s)
	while ! serial "test \$(grep -c '$1' $log) -ge $2" >/dev/null 2>&1; do
		if [ $(( $(date +%s) - started )) -ge "$3" ]; then
			return 1
		fi
		sleep 1
	done
}

# Both heads have a size before the compositor starts (QEMU ignores a size equal to the last one, and an unplug of a
# head never given one): display 0 1280x800 is the one the compositor starts on, display 1 1024x768 a head.
head_set 0 1280x800
head_set 1 1024x768
sleep 3

# The compositor, in the background, its log in a guest file.
serial "rm -rf /tmp/p4; mkdir -p /tmp/p4/home; XDG_RUNTIME_DIR=/tmp/p4 HOME=/tmp/p4/home /bin/wayland --testing --timeout=1200 > $log 2>&1 < /dev/null &" \
    > "$out/compositor-start.txt" 2>&1 || true

# The start: window mode, the hotplug thread's first fence, the two displays read, the hotplug thread running.
if wait_count 'KWL MODE window' 1 120 && wait_count 'KWL OUTPUT hotplug first' 1 30 && wait_count 'KWL OUTPUT displays count=2' 1 30; then
	say "start: PASS window mode, the first fence told, two displays read"
else
	fail_step start "no window mode, first fence or two displays (see $out/kwl.log)"
fi
if wait_count 'KWL THREADS .*hotplug' 1 10; then
	say "threads: PASS hotplug in KWL THREADS"
else
	fail_step threads "no hotplug in KWL THREADS"
fi
wait_count 'KWL OUTPUT head open' 1 30 || true
sleep 5

# 60 s with no change: no hotplug told, no reading of the displays.
signals=$(count 'KWL OUTPUT hotplug signal')
lists=$(count 'KWL OUTPUT displays count=')
sleep 60
signals_after=$(count 'KWL OUTPUT hotplug signal')
lists_after=$(count 'KWL OUTPUT displays count=')
if [ "$signals" -ge 0 ] && [ "$signals_after" -eq "$signals" ] && [ "$lists_after" -eq "$lists" ]; then
	say "idle-60: PASS hotplug told ${signals}->${signals_after}, displays read ${lists}->${lists_after}"
else
	fail_step idle-60 "hotplug told ${signals}->${signals_after}, displays read ${lists}->${lists_after}"
fi

# One change of a head, then the lines it brings, timed from the host's change (to the serial check's resolution).
change() {
	name=$1
	head=$2
	size=$3
	shift 3
	before=$(count 'KWL OUTPUT hotplug signal')
	changed_at=$(now_ms)
	head_set "$head" "$size"
	if ! wait_count 'KWL OUTPUT hotplug signal' $((before + 1)) 30; then
		fail_step "$name" "no hotplug told after head $head $size"
		return 0
	fi
	seen=$(( $(now_ms) - changed_at ))
	while [ "$#" -ge 2 ]; do
		if ! wait_count "$1" "$2" 30; then
			fail_step "$name" "fewer than $2 lines '$1' after head $head $size"
			return 0
		fi
		shift 2
	done
	say "$name: PASS head $head $size told within ${seen} ms of the change"
}

# Head 1 unplugged and plugged: its head closes and opens again.
opened=$(count 'KWL OUTPUT head open')
closed=$(count 'KWL OUTPUT head closed')
change unplug-1 1 off 'KWL OUTPUT head closed' $((closed + 1)) 'KWL OUTPUT displays count=1' 1
change plug-1 1 1024x768 'KWL OUTPUT head open' $((opened + 1))

# Head 0, the display shown, unplugged: the output moves to display 1; plugged again: it stays, display 0 a head.
switches=$(count 'KWL OUTPUT switch name=')
opened=$(count 'KWL OUTPUT head open')
change unplug-0 0 off 'KWL OUTPUT switch name=' $((switches + 1))
change plug-0 0 1280x800 'KWL OUTPUT head open' $((opened + 1))
sleep 5
switches_after=$(count 'KWL OUTPUT switch name=')
if [ "$switches_after" -eq $((switches + 1)) ]; then
	say "stay: PASS the output stays on display 1 after display 0 comes back"
else
	fail_step stay "the output moved ${switches}->${switches_after} times"
fi

# 30 s with no change again.
signals=$(count 'KWL OUTPUT hotplug signal')
lists=$(count 'KWL OUTPUT displays count=')
sleep 30
signals_after=$(count 'KWL OUTPUT hotplug signal')
lists_after=$(count 'KWL OUTPUT displays count=')
if [ "$signals_after" -eq "$signals" ] && [ "$lists_after" -eq "$lists" ]; then
	say "idle-30: PASS hotplug told ${signals}->${signals_after}, displays read ${lists}->${lists_after}"
else
	fail_step idle-30 "hotplug told ${signals}->${signals_after}, displays read ${lists}->${lists_after}"
fi

# The end: SIGTERM, the hotplug thread stopped and joined, the compositor gone within 20 s.
serial 'for p in $(ps -A -o pid,args | grep "[/]bin/wayland" | while read a b; do echo $a; done); do kill $p; done' \
    > "$out/kill.txt" 2>&1 || true
if wait_count 'KWL OUTPUT hotplug stopped' 1 20 && wait_count 'KWL EXIT' 1 20; then
	say "stop: PASS the hotplug thread stopped and the compositor ended"
else
	fail_step stop "no hotplug stopped or KWL EXIT within 20 s"
fi
serial "grep 'KWL OUTPUT hotplug stopped' $log" > "$out/stopped.txt" 2>&1 || true
wakes=$(tr -d '\r' < "$out/stopped.txt" | sed -n 's/^KWL OUTPUT hotplug stopped wakes=\([0-9]*\) looks=\([0-9]*\).*/\1/p' | sed -n '$p')
looks=$(tr -d '\r' < "$out/stopped.txt" | sed -n 's/^KWL OUTPUT hotplug stopped wakes=\([0-9]*\) looks=\([0-9]*\).*/\2/p' | sed -n '$p')
signals=$(count 'KWL OUTPUT hotplug signal')
if [ -n "$wakes" ] && [ "$wakes" -ge $((signals + 1)) ]; then
	say "wakes: PASS the hotplug thread woke ${wakes} times for ${signals} hotplugs told and the stop; the follow read the displays ${looks} times"
else
	fail_step wakes "wakes '${wakes}' for ${signals} hotplugs told"
fi
failures=$(count 'KWL FAILED')
if [ "$failures" -eq 0 ]; then
	say "failed: PASS no KWL FAILED"
else
	fail_step failed "${failures} KWL FAILED lines"
fi

# The guest's log for the record (read over the serial line: kernel records may be mixed in; not used for the verdict).
serial "grep -e 'KWL OUTPUT' -e 'KWL MODE' -e 'KWL THREADS' -e 'KWL EXIT' -e 'KWL FAILED' -e 'KWL DISPLAY ' $log" > "$out/kwl.log" 2>&1 || true
serial 'dmesg | grep -i -e venus -e gpu' > "$out/dmesg-gpu.txt" 2>&1 || true

if [ "$failed" -eq 0 ]; then
	say "qemu-hotplug-follow: PASS"
else
	say "qemu-hotplug-follow: FAIL"
	exit 1
fi

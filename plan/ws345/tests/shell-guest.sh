#!/bin/sh
# ws345-p007: the browser's window on the zedBSD QEMU guest (Venus GPU) stays alive while its page's script runs:
# one script, image -> boot -> run -> collect -> summary.  The image is plan/ws074/tests/config-amd64-browser.mk
# built from this tree with the guest harness, the ws074 test pages and this directory's pages/ (unless IMAGE is
# given).  The guest is plan/ws035/tests/zdesktop-guest.sh's Venus guest in its own runtime directory
# (GUEST_RUNTIME, default build/ws345-p007-run; its SSH port is a free one guest.py picks).  For each case the
# compositor runs at 1280x800 and the browser opens the page at 900x600 with BROWSER_LATENCY set, and while the
# page's script is running (its "start" console line came and its "end" did not):
#  A. the titlebar's reload is clicked: the page is shown again (NAVIGATE) and its script starts again;
#  B. the window's bottom-right corner is dragged 150 x 40 outwards (the pointer stays on the 1280x800 output): a
#     frame of the size the compositor settled on (KWL RESIZE settled) is drawn while the script still runs;
#  C. Ctrl+L, a path and Enter: the typed page is shown (NAVIGATE .../about.html);
#  D. Ctrl+Q closes the window; the browser's LATENCY line (the window's thread's longest wait for an event,
#     shell/latency.c) has no wait over 100 ms.
# Cases: busy (busy.html, a 30 s loop, GPU), xhr (hung-xhr.html, a synchronous XMLHttpRequest to 192.0.2.1, GPU),
# busy-cpu (busy.html with --no-gpu).  Every step's screenshot (VNC of the Venus output, not the console) is in OUT.
# The judgement uses the browser's and the compositor's logs read over SSH and the screenshots; never the serial
# console.  Last line: shell-guest: PASS or shell-guest: FAIL.
#
#   sh plan/ws345/tests/shell-guest.sh [OUT] [IMAGE]
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
set -u
cd "$(dirname -- "$0")/../../.."
root=$(pwd)
. plan/tools/fresh-out.sh
out=${1:-build/tmp/ws345-p007-guest}
fresh_out "$out"
out=$root/$out
image=${2:-}
GUEST_RUNTIME="${GUEST_RUNTIME:-$root/build/ws345-p007-run}"
export GUEST_RUNTIME
build=build/ws345-p007-image
pages=/usr/share/browser-tests
status=0
passed=0
failed=0

guest() { timeout 120 python3 plan/tools/guest/guest.py run "$1" 2>&1 </dev/null; }
pointer() { python3 plan/ws035/tests/qmp-pointer.py "$GUEST_RUNTIME/qmp.sock" "$@"; }
keys() { python3 plan/ws035/tests/qmp-keys.py "$GUEST_RUNTIME/qmp.sock" "$@"; }
shot() { timeout 60 python3 plan/ws035/tests/zdesktop-check.py "$out/$1.png" --runtime "$GUEST_RUNTIME" >/dev/null 2>&1; }
stop_all='for p in $(ps -A -o pid,args | grep -E "[w]ayland( |$)|[b]rowser" | awk "{print \$1}"); do kill $p; done; i=0; while ps -A -o args | grep -qE "[w]ayland( |$)|[b]rowser" && [ $i -lt 50 ]; do sleep 0.2; i=$((i+1)); done'

# The result of an item: result PASS|FAIL <case> <item> <text>.
result() {
	printf 'shell-guest: %s: %s %s: %s\n' "$1" "$2" "$3" "$4" | tee -a "$out/summary.txt"
	if [ "$1" = PASS ]; then
		passed=$((passed + 1))
	else
		failed=$((failed + 1))
		status=1
	fi
}

# The number of lines of a guest log matching a pattern.
count() {
	guest "grep -cE '$2' $1" | tail -1
}

# The last line of a guest log matching a pattern.
last() {
	guest "grep -E '$2' $1 | tail -1" | tail -1
}

# Waits up to SECONDS for a guest log to have more than BEFORE lines matching a pattern: wait_more LOG PATTERN BEFORE SECONDS.
wait_more() {
	tries=0
	while [ $tries -lt "$4" ]; do
		found=$(count "$1" "$2")
		[ "${found:-0}" -gt "$3" ] 2>/dev/null && return 0
		tries=$((tries + 1))
		sleep 1
	done
	return 1
}

# One case: run_case NAME PAGE START END [BROWSER OPTION].
run_case() {
	name=$1 page=$2 start=$3 end=$4 option=${5:-}
	guest "$stop_all" >/dev/null
	guest 'export XDG_RUNTIME_DIR=/tmp; rm -f /tmp/wayland-0
picture=; [ -f /usr/share/keiland/wallpaper.png ] && picture=--wallpaper=/usr/share/keiland/wallpaper.png
/bin/wayland --testing --timeout=900 --width=1280 --height=800 --glass $picture > /tmp/zdesktop.log 2>&1 </dev/null & w=0; while [ $w -lt 120 ]; do grep -q KWL.READY /tmp/zdesktop.log 2>/dev/null && break; sleep 0.5; w=$((w+1)); done; sleep 1; echo started' >/dev/null
	guest "export XDG_RUNTIME_DIR=/tmp BROWSER_LATENCY=1; /bin/browser $option --width=900 --height=600 $pages/$page > /tmp/b.log 2>&1 </dev/null & echo started" >/dev/null
	if ! wait_more /tmp/b.log 'ZBROWSER READY width=900 height=600' 0 30; then
		result FAIL "$name" ready "no READY line"
		guest 'cat /tmp/b.log' > "$out/$name-browser.log"
		return
	fi
	wait_more /tmp/zdesktop.log 'KWL TITLEBAR control client=[0-9]+ surface=[0-9]+ where=[a-z]* id=3 ' 0 20
	sleep 2
	set -- $(last /tmp/zdesktop.log 'KWL MAP client=' | sed -n 's/.*client=\([0-9]*\) surface=\([0-9]*\) x=\(-*[0-9]*\) y=\(-*[0-9]*\).*/\1 \2 \3 \4/p')
	client=${1:-0} surface=${2:-0} wx=${3:-0} wy=${4:-0}
	echo "$name: window client $client surface $surface at $wx,$wy" >> "$out/notes.txt"

	# The page's script runs.
	if ! wait_more /tmp/b.log "ZBROWSER CONSOLE level=0 $start\$" 0 20; then
		result FAIL "$name" script "no '$start' line"
	fi
	pointer move $((wx + 450)) $((wy + 300)) sleep 300 move $((wx + 460)) $((wy + 310)) sleep 300
	shot "$name-1-running"

	# A. The reload control, clicked while the script runs.
	navigations=$(count /tmp/b.log 'ZBROWSER NAVIGATE ')
	starts=$(count /tmp/b.log "ZBROWSER CONSOLE level=0 $start\$")
	set -- $(last /tmp/zdesktop.log "KWL TITLEBAR control client=$client surface=$surface where=[a-z]* id=3 " | sed -n 's/.* x=\(-*[0-9]*\) y=\(-*[0-9]*\) width=\([0-9]*\) height=\([0-9]*\).*/\1 \2 \3 \4/p')
	if [ -z "${1:-}" ]; then
		result FAIL "$name" reload "no reload control in the compositor's log"
	else
		rx=$(($1 + $3 / 2)) ry=$(($2 + $4 / 2))
		pointer move $((rx - 2)) "$ry" sleep 150 move "$rx" "$ry" sleep 300 down sleep 60 up sleep 200
		begin=$(date +%s)
		if wait_more /tmp/b.log 'ZBROWSER NAVIGATE ' "$navigations" 8 && wait_more /tmp/b.log "ZBROWSER CONSOLE level=0 $start\$" "$starts" 8; then
			result PASS "$name" reload "reloaded and its script started again within $(($(date +%s) - begin)) s ($rx,$ry)"
		else
			result FAIL "$name" reload "no new NAVIGATE and '$start' within 16 s of the click at $rx,$ry"
		fi
	fi
	sleep 1
	shot "$name-2-reloaded"

	# B. The bottom-right corner dragged outwards while the script runs.
	ends=$(count /tmp/b.log "ZBROWSER CONSOLE level=0 $end")
	settles=$(count /tmp/zdesktop.log "KWL RESIZE settled surface=$surface ")
	px=$((wx + 900 + 3)) py=$((wy + 600 + 3))
	set -- move $((px - 6)) $((py - 6)) sleep 120 move "$px" "$py" sleep 250 down sleep 80
	step=1
	while [ $step -le 8 ]; do
		set -- "$@" move $((px + 150 * step / 8)) $((py + 40 * step / 8)) sleep 30
		step=$((step + 1))
	done
	pointer "$@" sleep 250 up sleep 150
	begin=$(date +%s)
	wait_more /tmp/zdesktop.log "KWL RESIZE settled surface=$surface " "$settles" 8
	set -- $(last /tmp/zdesktop.log "KWL RESIZE settled surface=$surface " | sed -n 's/.* width=\([0-9]*\) height=\([0-9]*\).*/\1 \2/p')
	size="${1:-0}x${2:-0}"
	if [ "${1:-900}" != 900 ] && wait_more /tmp/b.log "ZBROWSER FRAME .* width=${1:-0} height=${2:-0}\$" 0 8; then
		still=$(count /tmp/b.log "ZBROWSER CONSOLE level=0 $end")
		if [ "${still:-0}" -le "${ends:-0}" ] 2>/dev/null; then
			result PASS "$name" resize "settled at $size, a frame of it within $(($(date +%s) - begin)) s, the script still running"
		else
			result FAIL "$name" resize "the frame of $size came after the script ended"
		fi
	else
		result FAIL "$name" resize "no frame of the settled size $size within 8 s ($(last /tmp/zdesktop.log "KWL RESIZE (start|end|settled) surface=$surface "))"
	fi
	sleep 1
	shot "$name-3-resized"

	# C. The location typed while the script runs.
	pointer move $((wx + 450)) $((wy + 300)) sleep 300
	keys '<ctrl-l>'
	sleep 1
	keys '/usr/share/browser/about.html' '<ret>'
	if wait_more /tmp/b.log 'ZBROWSER NAVIGATE path=[^ ]*about\.html' 0 10; then
		result PASS "$name" location "$(last /tmp/b.log 'ZBROWSER NAVIGATE path=[^ ]*about\.html' | cut -c1-120)"
	else
		result FAIL "$name" location "no NAVIGATE of about.html within 10 s"
	fi
	sleep 1
	shot "$name-4-location"

	# D. Ctrl+Q, and the window's thread's waits.
	pointer move $((wx + 450)) $((wy + 300)) sleep 300
	keys '<ctrl-q>'
	wait_more /tmp/b.log 'ZBROWSER LATENCY ' 0 15
	latency=$(last /tmp/b.log 'ZBROWSER LATENCY ')
	over=$(echo "$latency" | sed -n 's/.* over_100ms=\([0-9]*\).*/\1/p')
	samples=$(echo "$latency" | sed -n 's/.* samples=\([0-9]*\).*/\1/p')
	if [ -n "$over" ] && [ "$over" = 0 ] && [ "${samples:-0}" -gt 100 ] 2>/dev/null; then
		result PASS "$name" latency "$latency"
	else
		result FAIL "$name" latency "'$latency'"
	fi
	errors=$(count /tmp/b.log 'ZBROWSER ERROR')
	if [ "${errors:-1}" = 0 ]; then
		result PASS "$name" errors "no ZBROWSER ERROR line"
	else
		result FAIL "$name" errors "$(last /tmp/b.log 'ZBROWSER ERROR')"
	fi
	guest 'cat /tmp/b.log' > "$out/$name-browser.log"
	guest 'grep -E "KWL (MAP|RESIZE|CONFIGURE|ERROR|GPU_ERROR)" /tmp/zdesktop.log' > "$out/$name-zdesktop.log"
	guest "$stop_all" >/dev/null
}

# The image, built from this tree.
if [ -z "$image" ]; then
	set --
	for page in plan/ws074/tests/pages/* plan/ws345/tests/pages/*; do
		[ -f "$page" ] && set -- "$@" --file "/usr/share/browser-tests/$(basename "$page")=$page"
	done
	if ! timeout 5400 flock /tmp/zedbsd-image-build.lock plan/tools/guest/test-image.sh plan/ws074/tests/config-amd64-browser.mk \
	    "$build" --file /usr/share/keiland/wallpaper.png=userland/desktop/wallpapers/Birch-Lake.png "$@" > "$out/image.log" 2>&1; then
		echo "shell-guest: the image did not build (log $out/image.log)"
		tail -5 "$out/image.log"
		echo "shell-guest: FAIL"
		exit 1
	fi
	sync
	image=$build/hdd-image.img
fi

# The guest.
timeout 300 plan/ws035/tests/zdesktop-guest.sh start "$image" > "$out/guest.log" 2>&1
if ! timeout 300 python3 plan/tools/guest/guest.py wait > "$out/wait.log" 2>&1; then
	echo "shell-guest: the guest did not come up (logs $out/guest.log, $out/wait.log)"
	timeout 120 plan/ws035/tests/zdesktop-guest.sh stop > /dev/null 2>&1
	echo "shell-guest: FAIL"
	exit 1
fi

# The cases.
run_case busy busy.html 'busy start' 'busy end'
run_case xhr hung-xhr.html 'xhr start' 'xhr end'
run_case busy-cpu busy.html 'busy start' 'busy end' --no-gpu

# The guest stopped, and the summary.
timeout 120 plan/ws035/tests/zdesktop-guest.sh stop > /dev/null 2>&1
sync
echo "shell-guest: $passed passed, $failed failed ($out)"
if [ $status -eq 0 ]; then
	echo "shell-guest: PASS"
else
	echo "shell-guest: FAIL"
fi
exit $status

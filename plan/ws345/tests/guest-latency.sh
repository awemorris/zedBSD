#!/bin/sh
# ws345: how long the browser's window thread leaves its events waiting while the page holds its script, on the
# zedBSD QEMU guest and under Wine: one script, image/build -> start -> run -> collect -> summary.  The page is
# pages/latency.html: a second after the load its timer's script makes a synchronous XMLHttpRequest to 192.0.2.1
# (TEST-NET-1, never answered: the loader gives up) and then loops for 10 s ("latency start", "latency loop",
# "latency end" on the console).  The browser runs with BROWSER_LATENCY set; the pointer is moved over the window
# every half second from "latency start" to "latency end"; then Ctrl+Q, and the shell's diagnostic line
# (ZBROWSER LATENCY samples=N longest_ms=X over_20ms=A over_100ms=B, shell/latency.c) is judged: longest_ms at most
# 20, over_100ms 0, more than 100 samples, and the page's "latency end" came.  Cases (GUEST_LATENCY_CASES, default
# all): qemu (Venus GPU), qemu-cpu (--no-gpu), wine (lavapipe), wine-cpu (--no-gpu).
#
# QEMU: the image is plan/ws345/tests/config-amd64-browser.mk built from this tree with the guest harness and the
# ws074 and ws345 test pages (as shell-guest.sh builds it, in the same build directory) unless IMAGE is given; the
# guest is plan/ws035/tests/zdesktop-guest.sh's Venus guest in GUEST_RUNTIME (default build/p3-latency-run); its
# lines are read over SSH (plan/tools/guest/guest.py), its input sent through QMP (plan/ws035/tests/qmp-*.py), never
# the serial console.  Wine: apps/browser-win32's x86_64 build (as shell-wine.sh builds it, in the same build
# directory) in its own Xvfb, its standard output read on the host; each Wine command under flock
# /tmp/zedbsd-wine.lock.  The logs and the screenshots are in OUT.  Last line: guest-latency: PASS or FAIL.
#
#   sh plan/ws345/tests/guest-latency.sh [OUT] [IMAGE]
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
set -u
cd "$(dirname -- "$0")/../../.."
root=$(pwd)
. plan/tools/fresh-out.sh
out=${1:-build/tmp/ws345-guest-latency}
fresh_out "$out"
out=$root/$out
image=${2:-}
GUEST_RUNTIME="${GUEST_RUNTIME:-$root/build/p3-latency-run}"
export GUEST_RUNTIME
cases=${GUEST_LATENCY_CASES:-qemu qemu-cpu wine wine-cpu}
pages=/usr/share/browser-tests
status=0
passed=0
failed=0

# The result of a case: result PASS|FAIL <case> <text>.
result() {
	printf 'guest-latency: %s: %s: %s\n' "$1" "$2" "$3" | tee -a "$out/summary.txt"
	if [ "$1" = PASS ]; then
		passed=$((passed + 1))
	else
		failed=$((failed + 1))
		status=1
	fi
}

# Judges a LATENCY line and whether the page's script ended: judge CASE LINE ENDED.
judge() {
	longest=$(echo "$2" | sed -n 's/.* longest_ms=\([0-9.]*\).*/\1/p')
	over=$(echo "$2" | sed -n 's/.* over_100ms=\([0-9]*\).*/\1/p')
	samples=$(echo "$2" | sed -n 's/.* samples=\([0-9]*\).*/\1/p')
	if [ "$3" != 1 ]; then
		result FAIL "$1" "the page's script did not end ('latency end' missing): '$2'"
	elif [ -n "$longest" ] && [ -n "$over" ] && [ "$over" = 0 ] && [ "${samples:-0}" -gt 100 ] 2>/dev/null &&
	    awk -v x="$longest" 'BEGIN { exit !(x <= 20) }'; then
		result PASS "$1" "$2"
	else
		result FAIL "$1" "'$2' (bounds: longest_ms <= 20, over_100ms 0, samples > 100)"
	fi
}

has() {
	for each in $cases; do
		case $each in "$1"*) return 0 ;; esac
	done
	return 1
}

# ---- QEMU ----
guest() { timeout 120 python3 plan/tools/guest/guest.py run "$1" 2>&1 </dev/null; }
pointer() { python3 plan/ws035/tests/qmp-pointer.py "$GUEST_RUNTIME/qmp.sock" "$@"; }
keys() { python3 plan/ws035/tests/qmp-keys.py "$GUEST_RUNTIME/qmp.sock" "$@"; }
shot() { timeout 60 python3 plan/ws035/tests/zdesktop-check.py "$out/$1.png" --runtime "$GUEST_RUNTIME" >/dev/null 2>&1; }
count() { guest "grep -cE '$2' $1" | tail -1; }
last() { guest "grep -E '$2' $1 | tail -1" | tail -1; }
stop_all='for p in $(ps -A -o pid,args | grep -E "[w]ayland( |$)|[b]rowser" | awk "{print \$1}"); do kill $p; done; i=0; while ps -A -o args | grep -qE "[w]ayland( |$)|[b]rowser" && [ $i -lt 50 ]; do sleep 0.2; i=$((i+1)); done'

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

# One QEMU case: qemu_case NAME [BROWSER OPTION].
qemu_case() {
	name=$1 option=${2:-}
	guest "$stop_all" >/dev/null
	guest 'export XDG_RUNTIME_DIR=/tmp; rm -f /tmp/wayland-0
/bin/wayland --testing --timeout=900 --width=1280 --height=800 > /tmp/zdesktop.log 2>&1 </dev/null & w=0; while [ $w -lt 120 ]; do grep -q KWL.READY /tmp/zdesktop.log 2>/dev/null && break; sleep 0.5; w=$((w+1)); done; sleep 1; echo started' >/dev/null
	guest "export XDG_RUNTIME_DIR=/tmp BROWSER_LATENCY=1; /bin/browser $option --width=900 --height=600 $pages/latency.html > /tmp/b.log 2>&1 </dev/null & echo started" >/dev/null
	if ! wait_more /tmp/b.log 'ZBROWSER READY' 0 30; then
		guest 'cat /tmp/b.log' > "$out/$name-browser.log"
		result FAIL "$name" "no READY line"
		guest "$stop_all" >/dev/null
		return
	fi
	sleep 2
	set -- $(last /tmp/zdesktop.log 'KWL MAP client=' | sed -n 's/.* x=\(-*[0-9]*\) y=\(-*[0-9]*\).*/\1 \2/p')
	wx=${1:-0} wy=${2:-0}
	wait_more /tmp/b.log 'CONSOLE level=0 latency start' 0 20 || echo "$name: no 'latency start'" >> "$out/notes.txt"
	shot "$name-1-start"

	# The pointer over the window every half second until the script's end (at most 70 s).
	ended=0
	step=0
	while [ $step -lt 70 ]; do
		pointer move $((wx + 300 + step % 2 * 40)) $((wy + 300)) sleep 500 move $((wx + 320)) $((wy + 320)) sleep 500
		if [ "$(count /tmp/b.log 'CONSOLE level=0 latency end')" = 1 ]; then
			ended=1
			break
		fi
		[ $step = 35 ] && shot "$name-2-loop"
		step=$((step + 1))
	done
	shot "$name-3-end"

	# Ctrl+Q, and the window thread's line.
	keys '<ctrl-q>'
	wait_more /tmp/b.log 'ZBROWSER LATENCY samples=' 0 15
	line=$(last /tmp/b.log 'ZBROWSER LATENCY samples=')
	guest 'cat /tmp/b.log' > "$out/$name-browser.log"
	guest "$stop_all" >/dev/null
	judge "$name" "$line" "$ended"
}

if has qemu; then
	if [ -z "$image" ]; then
		set --
		for page in plan/ws074/tests/pages/* plan/ws345/tests/pages/*; do
			[ -f "$page" ] && set -- "$@" --file "/usr/share/browser-tests/$(basename "$page")=$page"
		done
		if ! timeout 5400 flock /tmp/zedbsd-image-build.lock plan/tools/guest/test-image.sh plan/ws345/tests/config-amd64-browser.mk \
		    build/ws345-p007-image --file /usr/share/keiland/wallpaper.png=userland/desktop/wallpapers/Birch-Lake.png "$@" > "$out/image.log" 2>&1; then
			result FAIL qemu "the image did not build (log $out/image.log)"
		fi
		sync
		image=build/ws345-p007-image/hdd-image.img
	fi
	if [ -f "$image" ]; then
		timeout 300 plan/ws035/tests/zdesktop-guest.sh start "$image" > "$out/guest.log" 2>&1
		if timeout 300 python3 plan/tools/guest/guest.py wait > "$out/wait.log" 2>&1; then
			for each in $cases; do
				case $each in
				qemu) qemu_case qemu ;;
				qemu-cpu) qemu_case qemu-cpu --no-gpu ;;
				esac
			done
		else
			result FAIL qemu "the guest did not come up (logs $out/guest.log, $out/wait.log)"
		fi
		timeout 120 plan/ws035/tests/zdesktop-guest.sh stop > /dev/null 2>&1
		sync
	fi
fi

# ---- Wine ----
if has wine; then
	LLVM_MINGW=${LLVM_MINGW:-$HOME/opt/llvm-mingw-20261020-ucrt-ubuntu-22.04-x86_64}
	build=build/tmp/ws345-p007-win64
	sources=build/tmp/ws345-p007-win32-sources
	lock=/tmp/zedbsd-wine.lock
	lvp=/usr/share/vulkan/icd.d/lvp_icd.json
	prefix=$root/build/tmp/ws345-wine-gui
	mkdir -p "$sources"
	if ! timeout 3000 make --no-print-directory -f apps/browser-win32/Makefile WIN32_ARCH=x86_64 -j4 LLVM_MINGW="$LLVM_MINGW" \
	    BROWSER_WIN32_BUILD="$build" BROWSER_WIN32_SOURCES="$sources" all zip > "$out/make.log" 2>&1 || grep -q 'warning:' "$out/make.log"; then
		result FAIL wine "the Windows build did not build cleanly (log $out/make.log)"
	else
		sync
		zip=$(ls "$build"/OmniGrammaton-*-win64.zip | head -1)
		mkdir -p "$out/extracted"
		unzip -q "$zip" -d "$out/extracted"
		dist=$out/extracted/OmniGrammaton
		cp plan/ws345/tests/pages/latency.html "$dist/"
		if [ ! -f "$prefix/system.reg" ]; then
			WINEPREFIX=$prefix WINEDEBUG=-all timeout 600 flock "$lock" xvfb-run -a wineboot -i > "$out/prefix.log" 2>&1
			WINEPREFIX=$prefix timeout 120 wineserver -w
		fi
		# The window in its own Xvfb: case.sh <the program's directory> <the output> <the program's options...>.
		cat > "$out/case.sh" << 'EOF'
#!/bin/sh
dist=$1
out=$2
shift 2
cd "$dist" || exit 2
lines() { tr -d '\r' < "$out/first.out" 2> /dev/null | grep -c "$1"; }
more() {
	tries=0
	while [ $tries -lt "$3" ]; do
		[ "$(lines "$1")" -gt "$2" ] && return 0
		tries=$((tries + 1))
		sleep 1
	done
	return 1
}
BROWSER_LATENCY=1 WINEDEBUG=-all timeout 300 wine OmniGrammaton.exe "$@" > "$out/first.out" 2> "$out/first.err" &
program=$!
more '^ZBROWSER READY' 0 90 || echo "no READY" >> "$out/notes.txt"
sleep 2
window=
X=0
Y=0
WIDTH=0
for each in $(xdotool search --class omnigrammaton 2> /dev/null); do
	geometry=$(xdotool getwindowgeometry --shell "$each" 2> /dev/null)
	size=$(echo "$geometry" | sed -n 's/^WIDTH=//p')
	if [ -n "$size" ] && [ "$size" -gt "$WIDTH" ]; then
		eval "$geometry"
		window=$each
	fi
done
echo "window $window x $X y $Y width $WIDTH" >> "$out/notes.txt"
more 'CONSOLE level=0 latency start' 0 30 || echo "no latency start" >> "$out/notes.txt"
import -window root "$out/1-start.png"
step=0
while [ $step -lt 140 ]; do
	xdotool mousemove $((X + 300 + step % 2 * 40)) $((Y + 300))
	sleep 0.5
	[ "$(lines 'CONSOLE level=0 latency end')" -gt 0 ] && break
	[ $step = 70 ] && import -window root "$out/2-loop.png"
	step=$((step + 1))
done
import -window root "$out/3-end.png"
xdotool key ctrl+q
for second in $(seq 20); do
	kill -0 "$program" 2> /dev/null || break
	sleep 1
done
wineserver -k 2> /dev/null
wait
EOF
		for each in $cases; do
			case $each in
			wine) option= ;;
			wine-cpu) option=--no-gpu ;;
			*) continue ;;
			esac
			mkdir -p "$out/$each"
			WINEPREFIX=$prefix VK_ICD_FILENAMES=$lvp timeout 600 flock "$lock" xvfb-run -a -s "-screen 0 1280x800x24" \
			    sh "$out/case.sh" "$dist" "$out/$each" $option latency.html > "$out/$each/xvfb.log" 2>&1
			tr -d '\r' < "$out/$each/first.out" > "$out/$each/first.txt" 2> /dev/null
			ended=0
			grep -q 'CONSOLE level=0 latency end' "$out/$each/first.txt" && ended=1
			judge "$each" "$(grep '^ZBROWSER LATENCY samples=' "$out/$each/first.txt" | tail -1)" "$ended"
		done
	fi
fi

sync
echo "guest-latency: $passed passed, $failed failed ($out)"
if [ $status -eq 0 ]; then
	echo "guest-latency: PASS"
else
	echo "guest-latency: FAIL"
fi
exit $status

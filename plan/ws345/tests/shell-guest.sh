#!/bin/sh
# ws345-p007: the browser's window on the zedBSD QEMU guest (Venus GPU) stays alive while its page's script runs:
# one script, image -> boot -> run -> collect -> summary.  The image is plan/ws345/tests/config-amd64-browser.mk
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
# busy-cpu (busy.html with --no-gpu).
# ws345-p014, the question about a page that does not respond: spin.html's timer script runs 15 s ("spin start" ...
# "spin end"; "tick" lines every half second while no script runs):
#  E. the question shows 8 to 13 s after "spin start" (PROMPT shown), and goes by itself when the script ends
#     (PROMPT hidden reason=ended, then "spin end");
#  F. after the titlebar's reload, the question again, and its Stop script clicked: PROMPT stop error=0, the question
#     goes, no "spin end" comes and new "tick" lines do (the page runs its timers again);
#  G. after a reload, the question again, and Enter (Wait): PROMPT wait, the question goes, and the script ends by
#     itself ("spin end": 15 s, before a second question would come at 20 s);
#  and D as above.  Cases: prompt (GPU), prompt-cpu (--no-gpu).  SHELL_GUEST_CASES (default all) names the cases run.
# Every step's screenshot (VNC of the Venus output, not the console) is in OUT.
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

# Starts the compositor and the browser on a page: start_session NAME PAGE [BROWSER OPTION]; sets client, surface, wx and
# wy (the window's content on the output); nonzero when the browser did not come up.
start_session() {
	name=$1 page=$2 option=${3:-}
	guest "$stop_all" >/dev/null
	guest 'export XDG_RUNTIME_DIR=/tmp; rm -f /tmp/wayland-0
picture=; [ -f /usr/share/keiland/wallpaper.png ] && picture=--wallpaper=/usr/share/keiland/wallpaper.png
/bin/wayland --testing --timeout=900 --width=1280 --height=800 --glass $picture > /tmp/zdesktop.log 2>&1 </dev/null & w=0; while [ $w -lt 120 ]; do grep -q KWL.READY /tmp/zdesktop.log 2>/dev/null && break; sleep 0.5; w=$((w+1)); done; sleep 1; echo started' >/dev/null
	guest "export XDG_RUNTIME_DIR=/tmp BROWSER_LATENCY=1; /bin/browser $option --width=900 --height=600 $pages/$page > /tmp/b.log 2>&1 </dev/null & echo started" >/dev/null
	if ! wait_more /tmp/b.log 'ZBROWSER READY width=900 height=600' 0 30; then
		result FAIL "$name" ready "no READY line"
		guest 'cat /tmp/b.log' > "$out/$name-browser.log"
		return 1
	fi
	wait_more /tmp/zdesktop.log 'KWL TITLEBAR control client=[0-9]+ surface=[0-9]+ where=[a-z]* id=3 ' 0 20
	sleep 2
	set -- $(last /tmp/zdesktop.log 'KWL MAP client=' | sed -n 's/.*client=\([0-9]*\) surface=\([0-9]*\) x=\(-*[0-9]*\) y=\(-*[0-9]*\).*/\1 \2 \3 \4/p')
	client=${1:-0} surface=${2:-0} wx=${3:-0} wy=${4:-0}
	echo "$name: window client $client surface $surface at $wx,$wy" >> "$out/notes.txt"
	return 0
}

# Clicks the titlebar's reload control: reload NAME; nonzero when the compositor's log has none.
reload() {
	set -- $(last /tmp/zdesktop.log "KWL TITLEBAR control client=$client surface=$surface where=[a-z]* id=3 " | sed -n 's/.* x=\(-*[0-9]*\) y=\(-*[0-9]*\) width=\([0-9]*\) height=\([0-9]*\).*/\1 \2 \3 \4/p')
	[ -n "${1:-}" ] || return 1
	rx=$(($1 + $3 / 2)) ry=$(($2 + $4 / 2))
	pointer move $((rx - 2)) "$ry" sleep 150 move "$rx" "$ry" sleep 300 down sleep 60 up sleep 200
	return 0
}

# Ctrl+Q, and the window's thread's waits and the errors judged: finish NAME.
finish() {
	pointer move $((wx + 450)) $((wy + 300)) sleep 300
	keys '<ctrl-q>'
	wait_more /tmp/b.log 'ZBROWSER LATENCY ' 0 15
	latency=$(last /tmp/b.log 'ZBROWSER LATENCY ')
	over=$(echo "$latency" | sed -n 's/.* over_100ms=\([0-9]*\).*/\1/p')
	samples=$(echo "$latency" | sed -n 's/.* samples=\([0-9]*\).*/\1/p')
	if [ -n "$over" ] && [ "$over" = 0 ] && [ "${samples:-0}" -gt 100 ] 2>/dev/null; then
		result PASS "$1" latency "$latency"
	else
		result FAIL "$1" latency "'$latency'"
	fi
	errors=$(count /tmp/b.log 'ZBROWSER ERROR')
	if [ "${errors:-1}" = 0 ]; then
		result PASS "$1" errors "no ZBROWSER ERROR line"
	else
		result FAIL "$1" errors "$(last /tmp/b.log 'ZBROWSER ERROR')"
	fi
	guest 'cat /tmp/b.log' > "$out/$1-browser.log"
	guest 'grep -E "KWL (MAP|RESIZE|CONFIGURE|ERROR|GPU_ERROR)" /tmp/zdesktop.log' > "$out/$1-zdesktop.log"
	guest "$stop_all" >/dev/null
}

# One case: run_case NAME PAGE START END [BROWSER OPTION].
run_case() {
	name=$1 page=$2 start=$3 end=$4 option=${5:-}
	start_session "$name" "$page" "$option" || return

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
	finish "$name"
}

# Waits up to SECONDS for a new question (more PROMPT shown lines than BEFORE) and reads its Stop script button into
# sx and sy (the output's place of its middle): prompt_shown BEFORE SECONDS.
prompt_shown() {
	wait_more /tmp/b.log 'ZBROWSER PROMPT shown ' "$1" "$2" || return 1
	shown=$(last /tmp/b.log 'ZBROWSER PROMPT shown ')
	set -- $(echo "$shown" | sed -n 's/.* stop=\([0-9]*\),\([0-9]*\),\([0-9]*\),\([0-9]*\).*/\1 \2 \3 \4/p')
	[ -n "${1:-}" ] || return 1
	sx=$((wx + $1 + $3 / 2)) sy=$((wy + $2 + $4 / 2))
	return 0
}

# The question about a page that does not respond: run_prompt NAME [BROWSER OPTION] (E, F, G and D above).
run_prompt() {
	name=$1 option=${2:-}
	start_session "$name" spin.html "$option" || return

	# E. The question 10 s into the script, and gone at its end.
	if ! wait_more /tmp/b.log 'ZBROWSER CONSOLE level=0 spin start$' 0 20; then
		result FAIL "$name" script "no 'spin start' line"
	fi
	begin=$(date +%s)
	pointer move $((wx + 100)) $((wy + 500)) sleep 300
	if prompt_shown 0 16; then
		took=$(($(date +%s) - begin))
		if [ "$took" -ge 8 ] && [ "$took" -le 13 ]; then
			result PASS "$name" shown "the question $took s after 'spin start': $(echo "$shown" | cut -c1-150)"
		else
			result FAIL "$name" shown "the question $took s after 'spin start'"
		fi
	else
		result FAIL "$name" shown "no PROMPT shown within 16 s of 'spin start'"
	fi
	sleep 1
	shot "$name-1-shown"
	if wait_more /tmp/b.log 'ZBROWSER PROMPT hidden .*reason=ended' 0 10 && wait_more /tmp/b.log 'ZBROWSER CONSOLE level=0 spin end$' 0 5; then
		result PASS "$name" ended "the question went by itself at the script's end: $(last /tmp/b.log 'ZBROWSER PROMPT hidden ')"
	else
		result FAIL "$name" ended "no PROMPT hidden reason=ended and 'spin end' ($(last /tmp/b.log 'ZBROWSER PROMPT '))"
	fi
	sleep 1
	shot "$name-2-ended"

	# F. Reloaded, the question again, and Stop script clicked.
	starts=$(count /tmp/b.log 'ZBROWSER CONSOLE level=0 spin start$')
	shows=$(count /tmp/b.log 'ZBROWSER PROMPT shown ')
	ends=$(count /tmp/b.log 'ZBROWSER CONSOLE level=0 spin end$')
	if ! reload; then
		result FAIL "$name" stop "no reload control in the compositor's log"
	elif ! wait_more /tmp/b.log 'ZBROWSER CONSOLE level=0 spin start$' "$starts" 10 || ! prompt_shown "$shows" 16; then
		result FAIL "$name" stop "no new 'spin start' and question after the reload"
	else
		sleep 1
		shot "$name-3-asked"
		pointer move $((sx - 3)) "$sy" sleep 200 move "$sx" "$sy" sleep 300 down sleep 80 up sleep 200
		if wait_more /tmp/b.log 'ZBROWSER PROMPT stop task=[0-9]* error=0' 0 5 && wait_more /tmp/b.log 'ZBROWSER PROMPT hidden .*reason=answered' 0 3; then
			ticks=$(count /tmp/b.log 'ZBROWSER CONSOLE level=0 tick ')
			sleep 3
			after=$(count /tmp/b.log 'ZBROWSER CONSOLE level=0 tick ')
			still=$(count /tmp/b.log 'ZBROWSER CONSOLE level=0 spin end$')
			if [ "${after:-0}" -gt "${ticks:-0}" ] && [ "${still:-0}" = "${ends:-0}" ] 2>/dev/null; then
				result PASS "$name" stop "Stop script ($sx,$sy) ended the script: $(last /tmp/b.log 'ZBROWSER PROMPT stop '), $((after - ticks)) ticks in 3 s after, no 'spin end'"
			else
				result FAIL "$name" stop "after the stop: ticks $ticks -> $after, 'spin end' $ends -> $still"
			fi
		else
			result FAIL "$name" stop "no PROMPT stop error=0 and hidden reason=answered after the click at $sx,$sy ($(last /tmp/b.log 'ZBROWSER PROMPT '))"
		fi
	fi
	shot "$name-4-stopped"

	# G. Reloaded, the question again, and Enter (Wait): the script then ends by itself.
	starts=$(count /tmp/b.log 'ZBROWSER CONSOLE level=0 spin start$')
	shows=$(count /tmp/b.log 'ZBROWSER PROMPT shown ')
	ends=$(count /tmp/b.log 'ZBROWSER CONSOLE level=0 spin end$')
	if ! reload; then
		result FAIL "$name" wait "no reload control in the compositor's log"
	elif ! wait_more /tmp/b.log 'ZBROWSER CONSOLE level=0 spin start$' "$starts" 10 || ! prompt_shown "$shows" 16; then
		result FAIL "$name" wait "no new 'spin start' and question after the reload"
	else
		waits=$(count /tmp/b.log 'ZBROWSER PROMPT wait ')
		keys '<ret>'
		if wait_more /tmp/b.log 'ZBROWSER PROMPT wait ' "$waits" 5 && wait_more /tmp/b.log 'ZBROWSER CONSOLE level=0 spin end$' "$ends" 10; then
			again=$(count /tmp/b.log 'ZBROWSER PROMPT shown ')
			if [ "${again:-0}" = $((shows + 1)) ] 2>/dev/null; then
				result PASS "$name" wait "Enter waited ($(last /tmp/b.log 'ZBROWSER PROMPT wait ')) and the script ended by itself"
			else
				result FAIL "$name" wait "the question came again before the script's end ($again shown lines)"
			fi
		else
			result FAIL "$name" wait "no PROMPT wait and 'spin end' after Enter ($(last /tmp/b.log 'ZBROWSER PROMPT '))"
		fi
	fi
	shot "$name-5-waited"

	# D. Ctrl+Q, the waits and the errors.
	finish "$name"
}

# The image, built from this tree.
if [ -z "$image" ]; then
	set --
	for page in plan/ws074/tests/pages/* plan/ws345/tests/pages/*; do
		[ -f "$page" ] && set -- "$@" --file "/usr/share/browser-tests/$(basename "$page")=$page"
	done
	if ! timeout 5400 flock /tmp/zedbsd-image-build.lock plan/tools/guest/test-image.sh plan/ws345/tests/config-amd64-browser.mk \
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
cases=${SHELL_GUEST_CASES:-busy xhr busy-cpu prompt prompt-cpu}
for each in $cases; do
	case $each in
	busy) run_case busy busy.html 'busy start' 'busy end' ;;
	xhr) run_case xhr hung-xhr.html 'xhr start' 'xhr end' ;;
	busy-cpu) run_case busy-cpu busy.html 'busy start' 'busy end' --no-gpu ;;
	prompt) run_prompt prompt ;;
	prompt-cpu) run_prompt prompt-cpu --no-gpu ;;
	*) result FAIL "$each" case "no such case" ;;
	esac
done

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

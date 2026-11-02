#!/bin/sh
# ws345-p007: OmniGrammaton's window under Wine (Xvfb 1280x800, Mesa's lavapipe through Wine's Vulkan) stays alive
# while its page's script runs: one script, build -> start -> run -> collect -> summary.  The Windows build is
# apps/browser-win32's (x86_64, its zip extracted into a new directory, as plan/ws334/tests/wine-run.sh does); the
# pages are this directory's pages/.  For each case the program opens the page with BROWSER_LATENCY set and, while
# the page's script runs (its "start" console line came and its "end" did not):
#  A. the titlebar's Reload (the frame's control 93 pixels from the left) is clicked: NAVIGATE and the script again;
#  B. (a second start of the case) the right edge is dragged 126 pixels left (the system's modal size loop): a frame
#     of the new width is drawn (FRAME width < 1000) while the script still runs;
#  C. Ctrl+L, the about page's path and Enter: NAVIGATE .../about.html;
#  D. Ctrl+Q closes the window; the LATENCY line of the first start (the window's thread's longest wait,
#     shell/latency.c) has no wait over 100 ms (the second start's, with the size loop that holds the window's
#     thread in the system, is written to the summary, not judged).
# Cases: busy (busy.html, a 30 s loop, the GPU), xhr (hung-xhr.html, a synchronous XMLHttpRequest to 192.0.2.1),
# busy-cpu (busy.html with --no-gpu).
# ws345-p014, the question about a page that does not respond (cases prompt and prompt-cpu, --no-gpu): spin.html's
# timer script runs 15 s:
#  E. the question shows 8 to 13 s after "spin start" (PROMPT shown);
#  F. its Stop script clicked (the button's place from the PROMPT shown line, under the title bar): PROMPT stop
#     error=0, new "tick" lines in the 3 s after and no "spin end" (the page runs its timers again);
#  and D (the waits are judged as in the main mode).  SHELL_WINE_CASES (default all) names the cases run.  The screenshots and the logs are in OUT; the review copies in
# build/review/ws345-p007/.  Each Wine command runs under flock /tmp/zedbsd-wine.lock.  Last line: shell-wine: PASS
# or shell-wine: FAIL.
#
#   sh plan/ws345/tests/shell-wine.sh [OUT]
# LLVM_MINGW=<root> chooses llvm-mingw (default $HOME/opt/llvm-mingw-20261020-ucrt-ubuntu-22.04-x86_64).  The Wine
# prefix is build/tmp/ws345-wine-gui (made when missing).
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
set -u
cd "$(dirname -- "$0")/../../.."
root=$(pwd)
. plan/tools/fresh-out.sh
out=${1:-build/tmp/ws345-p007-wine}
fresh_out "$out"
out=$root/$out
LLVM_MINGW=${LLVM_MINGW:-$HOME/opt/llvm-mingw-20261020-ucrt-ubuntu-22.04-x86_64}
build=build/tmp/ws345-p007-win64
sources=build/tmp/ws345-p007-win32-sources
review=build/review/ws345-p007
lock=/tmp/zedbsd-wine.lock
lvp=/usr/share/vulkan/icd.d/lvp_icd.json
prefix=$root/build/tmp/ws345-wine-gui
mkdir -p "$sources" "$review"
status=0
passed=0
failed=0

# The result of an item: result PASS|FAIL <case> <item> <text>.
result() {
	printf 'shell-wine: %s: %s %s: %s\n' "$1" "$2" "$3" "$4" | tee -a "$out/summary.txt"
	if [ "$1" = PASS ]; then
		passed=$((passed + 1))
	else
		failed=$((failed + 1))
		status=1
	fi
}

# The Windows build and its zip, extracted.
if ! timeout 3000 make --no-print-directory -f apps/browser-win32/Makefile WIN32_ARCH=x86_64 -j8 LLVM_MINGW="$LLVM_MINGW" \
    BROWSER_WIN32_BUILD="$build" BROWSER_WIN32_SOURCES="$sources" all zip > "$out/make.log" 2>&1 || grep -q 'warning:' "$out/make.log"; then
	echo "shell-wine: the Windows build did not build cleanly (log $out/make.log)"
	echo "shell-wine: FAIL"
	exit 1
fi
sync
zip=$(ls "$build"/OmniGrammaton-*-win64.zip | head -1)
mkdir -p "$out/extracted"
unzip -q "$zip" -d "$out/extracted" || { echo "shell-wine: cannot extract $zip"; echo "shell-wine: FAIL"; exit 1; }
dist=$out/extracted/OmniGrammaton
cp plan/ws345/tests/pages/busy.html plan/ws345/tests/pages/hung-xhr.html plan/ws345/tests/pages/spin.html "$dist/"

# The prefix, made once.
if [ ! -f "$prefix/system.reg" ]; then
	WINEPREFIX=$prefix WINEDEBUG=-all timeout 600 flock "$lock" xvfb-run -a wineboot -i > "$out/prefix.log" 2>&1
	WINEPREFIX=$prefix timeout 120 wineserver -w
fi

# One window in its own Xvfb: case.sh <the program's directory> <the output> <mode> <start> <end> <the program's
# options...>.  The mode main does A, C and D; resize does B and D (the size loop holds the window's thread in the
# system, so its own wait is not the browser's: its LATENCY line is kept, not judged).
cat > "$out/case.sh" << 'EOF'
#!/bin/sh
dist=$1
out=$2
mode=$3
start=$4
end=$5
shift 5
cd "$dist" || exit 2
xset r off 2> /dev/null
lines() { tr -d '\r' < "$out/first.out" 2> /dev/null | grep -c "$1"; }
# Waits up to SECONDS for more than BEFORE lines matching a pattern: more PATTERN BEFORE SECONDS.
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
window=
X=0
Y=0
WIDTH=0
HEIGHT=0
sleep 2
for each in $(xdotool search --class omnigrammaton 2> /dev/null); do
	geometry=$(xdotool getwindowgeometry --shell "$each" 2> /dev/null)
	size=$(echo "$geometry" | sed -n 's/^WIDTH=//p')
	if [ -n "$size" ] && [ "$size" -gt "$WIDTH" ]; then
		eval "$geometry"
		window=$each
	fi
done
echo "window $window x $X y $Y width $WIDTH height $HEIGHT" >> "$out/notes.txt"
more "CONSOLE level=0 $start" 0 30 || echo "no $start" >> "$out/notes.txt"
xdotool mousemove $((X + 400)) $((Y + 300))
sleep 1
import -window root "$out/1-running.png"
if [ "$mode" = main ]; then
# A. Reload.
navigations=$(lines '^ZBROWSER NAVIGATE ')
starts=$(lines "CONSOLE level=0 $start")
begin=$(date +%s)
xdotool mousemove $((X + 93)) $((Y + 16)) click 1
if more '^ZBROWSER NAVIGATE ' "$navigations" 10 && more "CONSOLE level=0 $start" "$starts" 10; then
	echo "reload ok $(($(date +%s) - begin))" >> "$out/steps.txt"
else
	echo "reload missing" >> "$out/steps.txt"
fi
sleep 1
import -window root "$out/2-reloaded.png"
fi
if [ "$mode" = resize ]; then
# B. The right edge dragged left (the system's modal size loop).
ends=$(lines "CONSOLE level=0 $end")
xdotool mousemove $((X + WIDTH - 3)) $((Y + 400))
sleep 1
xdotool mousedown 1
sleep 1
for step in 1 2 3 4 5 6; do
	xdotool mousemove_relative -- -21 0
	sleep 0.3
done
import -window root "$out/3-resizing.png"
xdotool mouseup 1
sleep 3
frame=$(tr -d '\r' < "$out/first.out" | grep '^ZBROWSER FRAME' | tail -1)
echo "resize frame $frame ends-before $ends ends-after $(lines "CONSOLE level=0 $end")" >> "$out/steps.txt"
import -window root "$out/3-resized.png"
fi
if [ "$mode" = main ]; then
# C. The location.
xdotool mousemove $((X + 300)) $((Y + 300))
xdotool key ctrl+l
sleep 1
xdotool type --delay 40 "Z:$(pwd -P)/share/browser/about.html"
xdotool key Return
if more '^ZBROWSER NAVIGATE path=.*about\.html' 0 10; then
	echo "location ok" >> "$out/steps.txt"
else
	echo "location missing" >> "$out/steps.txt"
fi
sleep 1
import -window root "$out/4-location.png"
fi
if [ "$mode" = prompt ]; then
# E. The question 10 s into the script.
xdotool mousemove $((X + 100)) $((Y + 500))
begin=$(date +%s)
if more '^ZBROWSER PROMPT shown ' 0 16; then
	echo "shown $(($(date +%s) - begin)) $(tr -d '\r' < "$out/first.out" | grep '^ZBROWSER PROMPT shown ' | tail -1)" >> "$out/steps.txt"
else
	echo "shown missing" >> "$out/steps.txt"
fi
sleep 1
import -window root "$out/5-asked.png"
# F. Stop script, at its place under the title bar (the window's height less the content's).
height=$(tr -d '\r' < "$out/first.out" | grep -m1 '^ZBROWSER READY' | sed -n 's/.* height=\([0-9]*\).*/\1/p')
top=$((HEIGHT - ${height:-$HEIGHT}))
set -- $(tr -d '\r' < "$out/first.out" | grep '^ZBROWSER PROMPT shown ' | tail -1 | sed -n 's/.* stop=\([0-9]*\),\([0-9]*\),\([0-9]*\),\([0-9]*\).*/\1 \2 \3 \4/p')
if [ -n "${1:-}" ]; then
	sx=$((X + $1 + $3 / 2))
	sy=$((Y + top + $2 + $4 / 2))
	ticks=$(lines 'CONSOLE level=0 tick ')
	xdotool mousemove $((sx - 3)) "$sy"
	sleep 0.3
	xdotool mousemove "$sx" "$sy"
	sleep 0.3
	xdotool click 1
	if more '^ZBROWSER PROMPT stop task=[0-9]* error=0' 0 5; then
		sleep 3
		echo "stop ok at $sx,$sy ticks $ticks $(lines 'CONSOLE level=0 tick ') ends $(lines 'CONSOLE level=0 spin end')" >> "$out/steps.txt"
	else
		echo "stop missing at $sx,$sy" >> "$out/steps.txt"
	fi
else
	echo "stop no button" >> "$out/steps.txt"
fi
import -window root "$out/6-stopped.png"
fi
# D. Ctrl+Q.
xdotool key ctrl+q
for second in $(seq 20); do
	kill -0 "$program" 2> /dev/null || break
	sleep 1
done
wineserver -k 2> /dev/null
wait
EOF

# One case: run_case NAME PAGE START END [OPTION].
run_case() {
	name=$1 page=$2 start=$3 end=$4
	shift 4
	mkdir -p "$out/$name" "$out/$name-resize"
	WINEPREFIX=$prefix VK_ICD_FILENAMES=$lvp timeout 600 flock "$lock" xvfb-run -a -s "-screen 0 1280x800x24" \
	    sh "$out/case.sh" "$dist" "$out/$name" main "$start" "$end" "$@" "$page" > "$out/$name/xvfb.log" 2>&1
	WINEPREFIX=$prefix VK_ICD_FILENAMES=$lvp timeout 600 flock "$lock" xvfb-run -a -s "-screen 0 1280x800x24" \
	    sh "$out/case.sh" "$dist" "$out/$name-resize" resize "$start" "$end" "$@" "$page" > "$out/$name-resize/xvfb.log" 2>&1
	for run in "$name" "$name-resize"; do
		tr -d '\r' < "$out/$run/first.out" > "$out/$run/first.txt" 2> /dev/null
		for picture in "$out/$run"/*.png; do
			[ -f "$picture" ] && cp "$picture" "$review/$run-$(basename "$picture")"
		done
	done
	cat "$out/$name-resize/steps.txt" >> "$out/$name/steps.txt" 2> /dev/null
	steps=$out/$name/steps.txt
	if grep -q '^reload ok' "$steps" 2> /dev/null; then
		result PASS "$name" reload "reloaded and its script started again within $(sed -n 's/^reload ok //p' "$steps") s"
	else
		result FAIL "$name" reload "$(cat "$out/$name/notes.txt" "$steps" 2> /dev/null | tr '\n' ';')"
	fi
	line=$(grep '^resize ' "$steps" 2> /dev/null)
	width=$(echo "$line" | sed -n 's/.* width=\([0-9]*\) .*/\1/p')
	before=$(echo "$line" | sed -n 's/.* ends-before \([0-9]*\) .*/\1/p')
	after=$(echo "$line" | sed -n 's/.* ends-after \([0-9]*\).*/\1/p')
	if [ -n "$width" ] && [ "$width" -lt 1000 ] && [ "$width" -gt 850 ] && [ "$before" = "$after" ]; then
		result PASS "$name" resize "$line"
	else
		result FAIL "$name" resize "'$line'"
	fi
	if grep -q '^location ok' "$steps" 2> /dev/null; then
		result PASS "$name" location "$(grep -m1 '^ZBROWSER NAVIGATE path=.*about\.html' "$out/$name/first.txt" | cut -c1-140)"
	else
		result FAIL "$name" location "no NAVIGATE of about.html"
	fi
	latency=$(grep '^ZBROWSER LATENCY ' "$out/$name/first.txt" | tail -1)
	over=$(echo "$latency" | sed -n 's/.* over_100ms=\([0-9]*\).*/\1/p')
	samples=$(echo "$latency" | sed -n 's/.* samples=\([0-9]*\).*/\1/p')
	if [ "$over" = 0 ] && [ "${samples:-0}" -gt 100 ] 2> /dev/null; then
		result PASS "$name" latency "$latency"
	else
		result FAIL "$name" latency "'$latency'"
	fi
	echo "$name-resize: $(grep '^ZBROWSER LATENCY' "$out/$name-resize/first.txt" | tr '\n' ';')" >> "$out/summary.txt"
	if grep -q '^ZBROWSER ERROR' "$out/$name/first.txt" "$out/$name-resize/first.txt"; then
		result FAIL "$name" errors "$(grep -h -m1 '^ZBROWSER ERROR' "$out/$name/first.txt" "$out/$name-resize/first.txt" | head -1)"
	else
		result PASS "$name" errors "no ZBROWSER ERROR line"
	fi
}

# One case of the question: run_prompt NAME [OPTION].
run_prompt() {
	name=$1
	shift
	mkdir -p "$out/$name"
	WINEPREFIX=$prefix VK_ICD_FILENAMES=$lvp timeout 600 flock "$lock" xvfb-run -a -s "-screen 0 1280x800x24" \
	    sh "$out/case.sh" "$dist" "$out/$name" prompt 'spin start' 'spin end' "$@" spin.html > "$out/$name/xvfb.log" 2>&1
	tr -d '\r' < "$out/$name/first.out" > "$out/$name/first.txt" 2> /dev/null
	for picture in "$out/$name"/*.png; do
		[ -f "$picture" ] && cp "$picture" "$review/$name-$(basename "$picture")"
	done
	steps=$out/$name/steps.txt
	line=$(grep '^shown ' "$steps" 2> /dev/null)
	took=$(echo "$line" | sed -n 's/^shown \([0-9]*\) .*/\1/p')
	if [ -n "$took" ] && [ "$took" -ge 8 ] && [ "$took" -le 13 ]; then
		result PASS "$name" shown "$(echo "$line" | cut -c1-160)"
	else
		result FAIL "$name" shown "'$line' $(cat "$out/$name/notes.txt" 2> /dev/null | tr '\n' ';')"
	fi
	line=$(grep '^stop ' "$steps" 2> /dev/null)
	before=$(echo "$line" | sed -n 's/.* ticks \([0-9]*\) .*/\1/p')
	after=$(echo "$line" | sed -n 's/.* ticks [0-9]* \([0-9]*\) .*/\1/p')
	ends=$(echo "$line" | sed -n 's/.* ends \([0-9]*\).*/\1/p')
	if [ -n "$after" ] && [ "$after" -gt "$before" ] && [ "$ends" = 0 ] 2> /dev/null; then
		result PASS "$name" stop "$line ($(grep -m1 '^ZBROWSER PROMPT stop ' "$out/$name/first.txt"))"
	else
		result FAIL "$name" stop "'$line'"
	fi
	latency=$(grep '^ZBROWSER LATENCY ' "$out/$name/first.txt" | tail -1)
	over=$(echo "$latency" | sed -n 's/.* over_100ms=\([0-9]*\).*/\1/p')
	samples=$(echo "$latency" | sed -n 's/.* samples=\([0-9]*\).*/\1/p')
	if [ "$over" = 0 ] && [ "${samples:-0}" -gt 100 ] 2> /dev/null; then
		result PASS "$name" latency "$latency"
	else
		result FAIL "$name" latency "'$latency'"
	fi
	if grep -q '^ZBROWSER ERROR' "$out/$name/first.txt"; then
		result FAIL "$name" errors "$(grep -m1 '^ZBROWSER ERROR' "$out/$name/first.txt")"
	else
		result PASS "$name" errors "no ZBROWSER ERROR line"
	fi
}

cases=${SHELL_WINE_CASES:-busy xhr busy-cpu prompt prompt-cpu}
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
sync
echo "shell-wine: $passed passed, $failed failed ($out)"
if [ $status -eq 0 ]; then
	echo "shell-wine: PASS"
else
	echo "shell-wine: FAIL"
fi
exit $status

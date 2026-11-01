#!/bin/sh
# OmniGrammaton under Wine on the host (x86_64; the reference, not the judgement of a real Windows):
#  0. the Windows build (make -f apps/browser-win32/Makefile: OmniGrammaton.exe and its directory) and a
#     test's console program over the browser's entry (plan/ws334/tests/browser-cli-main.c), and the host's
#     browser (plan/ws074/tests/host-build.sh plain);
#  1. the headless modes print the same bytes: --js of some of plan/ws074/tests/js, and --dump=layout of
#     the bundled start.html with the tree's fonts, from the console program under Wine and the host's;
#  2. OmniGrammaton.exe starts on start.html in Xvfb (1280x800) and draws it with Vulkan (Mesa's
#     lavapipe through Wine's Vulkan): a screenshot of the root window (gpu.png);
#  3. the same with --no-gpu (the CPU's frames): nogpu.png;
#  4. while the first runs, a second start with about.html hands it over (WM_COPYDATA) and ends with 0;
#     the first says so (ZBROWSER OPEN) and shows it: second.png;
#  5. the title bar's controls (xdotool): a press on the location's box edits it, about.html typed and
#     Enter opens it, Back returns to start.html; the bar's picture (titlebar.png);
#  6. Ctrl+L and Ctrl+C put the box's text on the clipboard (xclip reads Wine's CLIPBOARD);
#  7. at 144 dpi (a prefix with LogPixels 144) the title bar is 1.5 times as tall (titlebar-144dpi.png).
# The input method cannot be checked under Wine (a real Windows, later).
# A screenshot passes when it has more than 100 colours (a page, not an empty window).  The PNGs are
# copied to build/review/ws334-p028/ (2 to 4) and build/review/ws334-p029/ (5 to 7).  Wine runs under flock /tmp/zedbsd-wine.lock.
#   sh plan/ws334/tests/wine-run.sh
# LLVM_MINGW=<root> chooses llvm-mingw (default $HOME/opt/llvm-mingw-20261020-ucrt-ubuntu-22.04-x86_64);
# the Wine prefixes are build/tmp/ws334-wine (the console program) and build/tmp/ws334-wine-gui (the
# windows).  Last line: wine-run: PASS or wine-run: FAIL.
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
set -u
cd "$(dirname -- "$0")/../../.."
root=$(pwd)
. plan/tools/fresh-out.sh
out=build/tmp/ws334-wine-run
fresh_out "$out"
out=$root/$out
LLVM_MINGW=${LLVM_MINGW:-$HOME/opt/llvm-mingw-20261020-ucrt-ubuntu-22.04-x86_64}
build=build/tmp/ws334-wine-build/browser-win64
sources=build/tmp/ws334-win32-sources
review=build/review/ws334-p028
mkdir -p "$sources" "$review"
status=0

# 0. The builds.
if ! timeout 1800 make --no-print-directory -f apps/browser-win32/Makefile WIN32_ARCH=x86_64 -j8 \
    LLVM_MINGW="$LLVM_MINGW" BROWSER_WIN32_BUILD="$build" BROWSER_WIN32_SOURCES="$sources" all \
    > "$out/make.log" 2>&1 || grep -q 'warning:' "$out/make.log"; then
	echo "wine-run: the Windows build did not build cleanly (log $out/make.log)"
	tail -10 "$out/make.log"
	echo "wine-run: FAIL"
	exit 1
fi
if ! timeout 1800 make --no-print-directory -f apps/browser-win32/Makefile WIN32_ARCH=x86_64 -j8 \
    LLVM_MINGW="$LLVM_MINGW" BROWSER_WIN32_BUILD="$build" BROWSER_WIN32_SOURCES="$sources" \
    test-program TEST_PROGRAM=browser-cli TEST_PROGRAM_SOURCES=plan/ws334/tests/browser-cli-main.c \
    TEST_PROGRAM_LDFLAGS=-municode > "$out/make-cli.log" 2>&1 || grep -q 'warning:' "$out/make-cli.log"; then
	echo "wine-run: the console program did not build cleanly (log $out/make-cli.log)"
	tail -10 "$out/make-cli.log"
	echo "wine-run: FAIL"
	exit 1
fi
if ! timeout 3600 sh plan/ws074/tests/host-build.sh plain > "$out/host-build.log" 2>&1; then
	echo "wine-run: the host's browser did not build (log $out/host-build.log)"
	echo "wine-run: FAIL"
	exit 1
fi
cli=$build/tests/browser-cli.exe
host=build/ws074-host/plain/browser
dist=$root/$build/OmniGrammaton

# 1. The headless modes, the same bytes on both.
fonts="--font=userland/desktop/fonts/Mahora-Regular.ttf --mono-font=userland/desktop/fonts/Mahora-Mono.ttf"
fonts="$fonts --fallback-font=userland/desktop/fonts/DroidSansFallbackFull.ttf"
compare() {
	name=$1
	shift
	timeout 300 "$host" "$@" > "$out/$name.host" 2>&1
	WINEPREFIX=$root/build/tmp/ws334-wine WINEDEBUG=-all timeout 300 flock /tmp/zedbsd-wine.lock wine "$cli" "$@" \
	    > "$out/$name.wine" 2>&1
	if cmp -s "$out/$name.host" "$out/$name.wine" && [ -s "$out/$name.host" ]; then
		echo "wine-run: $name: the same $(wc -c < "$out/$name.host") bytes"
	else
		echo "wine-run: $name: the host and Wine differ ($out/$name.host, $out/$name.wine)"
		status=1
	fi
}
for test in closures builtins class; do
	compare "js-$test" --js "plan/ws074/tests/js/$test.js"
done
# shellcheck disable=SC2086
compare dump-layout --dump=layout $fonts userland/desktop/browser/data/start.html

# 2 to 7. The windows, in Xvfb, one Xvfb for each start.
cat > "$out/windows.sh" << 'EOF'
#!/bin/sh
# In Xvfb: $1 the program's directory, $2 the output, $3 the program's options (one word or none), $4 what
# else: "second" (a second start), "controls" (the title bar's controls, with xdotool) or nothing.
dist=$1
out=$2
options=$3
mode=$4
cd "$dist" || exit 2
WINEDEBUG=-all timeout 120 wine OmniGrammaton.exe $options share/browser/start.html > "$out/first.out" 2> "$out/first.err" &
sleep 25
import -window root "$out/first.png"
xdotool search --class omnigrammaton getwindowgeometry --shell 2> /dev/null | head -3 > "$out/geometry.txt"
if [ "$mode" = second ]; then
	WINEDEBUG=-all timeout 60 wine OmniGrammaton.exe share/browser/about.html > "$out/second.out" 2> "$out/second.err"
	echo "$?" > "$out/second.status"
	sleep 10
	import -window root "$out/second.png"
fi
if [ "$mode" = controls ]; then
	X=0
	Y=0
	. "$out/geometry.txt"
	# The location's box: a press edits it; a relative URL typed and Enter.
	xdotool mousemove $((X + 400)) $((Y + 16)) click 1
	sleep 2
	xdotool key ctrl+a
	xdotool type --delay 60 about.html
	sleep 2
	import -window root "$out/typing.png"
	xdotool key Return
	sleep 8
	import -window root "$out/typed.png"
	# The box's text to the clipboard (Ctrl+L edits it with all of it selected), then Escape.
	xdotool key ctrl+l
	sleep 2
	xdotool key ctrl+c
	sleep 2
	timeout 10 xclip -o -selection clipboard > "$out/clipboard.txt" 2> /dev/null
	xdotool key Escape
	sleep 2
	# Back.
	xdotool mousemove $((X + 25)) $((Y + 16)) click 1
	sleep 8
	import -window root "$out/back.png"
	# The URL the box showed, typed back (a file: URL of a drive's path opens).
	xdotool mousemove $((X + 400)) $((Y + 16)) click 1
	sleep 2
	xdotool key ctrl+a
	xdotool type --delay 60 "$(tr -d '\r\n' < "$out/clipboard.txt")"
	sleep 2
	xdotool key Return
	sleep 8
	import -window root "$out/retyped.png"
fi
wineserver -k 2> /dev/null
wait
EOF
colours() {
	convert "$1" -format %k info: 2> /dev/null || echo 0
}
pixel() {
	convert "$1" -format "%[pixel:p{$2,$3}]" info: 2> /dev/null
}
gui144=$root/build/tmp/ws334-wine-gui144
if [ ! -d "$gui144" ]; then
	WINEPREFIX=$gui144 WINEDEBUG=-all timeout 300 flock /tmp/zedbsd-wine.lock xvfb-run -a \
	    wine reg add 'HKCU\Control Panel\Desktop' /v LogPixels /t REG_DWORD /d 144 /f > "$out/gui144.log" 2>&1
fi
for run in gpu nogpu controls dpi144; do
	mkdir -p "$out/$run"
	options=
	mode=
	prefix=$root/build/tmp/ws334-wine-gui
	[ "$run" = nogpu ] && options=--no-gpu
	[ "$run" = gpu ] && mode=second
	[ "$run" = controls ] && mode=controls
	[ "$run" = dpi144 ] && prefix=$gui144
	WINEPREFIX=$prefix VK_ICD_FILENAMES=/usr/share/vulkan/icd.d/lvp_icd.json \
	    timeout 600 flock /tmp/zedbsd-wine.lock xvfb-run -a -s "-screen 0 1280x800x24" \
	    sh "$out/windows.sh" "$dist" "$out/$run" "$options" "$mode" > "$out/$run/xvfb.log" 2>&1
	tr -d '\r' < "$out/$run/first.out" > "$out/$run/first.txt" 2> /dev/null
	count=$(colours "$out/$run/first.png")
	if [ "$run" = gpu ] || [ "$run" = nogpu ]; then
		cp "$out/$run/first.png" "$review/$run.png" 2> /dev/null
	fi
	if [ "$count" -gt 100 ] && grep -q '^ZBROWSER READY' "$out/$run/first.txt"; then
		echo "wine-run: $run: the window drew start.html ($count colours)"
	else
		echo "wine-run: $run: no page in the window ($count colours; $out/$run/first.txt)"
		status=1
	fi
done

# 4. The second start.
second=$(cat "$out/gpu/second.status" 2> /dev/null)
count=$(colours "$out/gpu/second.png")
cp "$out/gpu/second.png" "$review/second.png" 2> /dev/null
if [ "$second" = 0 ] && grep -q '^ZBROWSER OPEN handed=.*about\.html$' "$out/gpu/first.txt" && [ "$count" -gt 100 ]; then
	printf "wine-run: second start: handed over and ended with 0; %s (%s)\n" "$(grep "^ZBROWSER OPEN" "$out/gpu/first.txt" | head -1)" "$review/second.png"
else
	echo "wine-run: second start: status '$second', the first did not say it opened about.html ($out/gpu/first.txt)"
	status=1
fi

# 5. The title bar's controls: the bar's picture, the typed URL followed, and Back.
review29=build/review/ws334-p029
mkdir -p "$review29"
convert "$out/controls/first.png" -crop 1024x32+0+0 "$review29/titlebar.png" 2> /dev/null
cp "$out/controls/typing.png" "$review29/typing.png" 2> /dev/null
cp "$out/controls/typed.png" "$review29/typed.png" 2> /dev/null
cp "$out/controls/back.png" "$review29/back.png" 2> /dev/null
navigations=$(grep '^ZBROWSER NAVIGATE path=' "$out/controls/first.txt" | sed 's/ title=.*//; s/.*[\\/]//')
typed=$(echo "$navigations" | sed -n 2p)
back=$(echo "$navigations" | sed -n 3p)
retyped=$(echo "$navigations" | sed -n 4p)
cp "$out/controls/retyped.png" "$review29/retyped.png" 2> /dev/null
if [ "$typed" = about.html ] && [ "$back" = start.html ] && [ "$retyped" = about.html ]; then
	echo "wine-run: title bar: the typed about.html opened, Back returned to start.html, the shown file: URL typed back opened about.html ($review29/titlebar.png, typed.png, back.png, retyped.png)"
else
	echo "wine-run: title bar: the pages opened were: $(echo $navigations) ($out/controls/first.txt)"
	status=1
fi

# 6. The clipboard.
if grep -q '^file:///[A-Za-z]:/.*about\.html' "$out/controls/clipboard.txt" 2> /dev/null; then
	printf "wine-run: clipboard: the box's text (a file: URL, with / as the separators) copied: %s\n" "$(head -c 200 "$out/controls/clipboard.txt" | tr -d '\r')"
else
	echo "wine-run: clipboard: the box's text is not a file: URL of the drive's path on the clipboard ($out/controls/clipboard.txt)"
	status=1
fi

# 7. 144 dpi: the title bar 48 pixels tall (at y 40 the bar's ground, as at y 5; at 96 dpi the page).
convert "$out/dpi144/first.png" -crop 1280x48+0+0 "$review29/titlebar-144dpi.png" 2> /dev/null
cp "$out/dpi144/first.png" "$review29/dpi144.png" 2> /dev/null
top=$(pixel "$out/dpi144/first.png" 700 5)
low=$(pixel "$out/dpi144/first.png" 700 40)
page=$(pixel "$out/gpu/first.png" 700 40)
if [ -n "$top" ] && [ "$top" = "$low" ] && [ "$page" != "$low" ]; then
	echo "wine-run: 144 dpi: the title bar is 48 pixels tall ($review29/titlebar-144dpi.png)"
else
	echo "wine-run: 144 dpi: the title bar is not 1.5 times as tall (y 5 $top, y 40 $low, 96 dpi y 40 $page)"
	status=1
fi
sync
if [ "$status" = 0 ]; then echo "wine-run: PASS"; else echo "wine-run: FAIL"; fi
exit "$status"

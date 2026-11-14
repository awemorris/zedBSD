#!/bin/sh
# OmniGrammaton from its zip under Wine on the host (x86_64) with Mesa's lavapipe (the CPU's Vulkan): the
# reference, not the judgement of a real Windows.  The items of the minimal set M1 to M8 that Wine can show
# (an item Wine cannot show is "not run (a real Windows)" and is not counted):
#  0. the Windows build (make -f apps/browser-win32/Makefile all zip: OmniGrammaton.exe and its zip), two
#     test's console programs over the same libraries (plan/tools/browser-win32/browser-cli-main.c over the
#     browser's entry, cert-store-main.c that puts a certificate into a store of the current user), the
#     host's browser (plan/ws074/tests/host-build.sh plain); the zip extracted into a new directory, and
#     every window below started from there;
#  M8 the headless modes print the same bytes on the host and under Wine: --js of all of plan/ws074/tests/js
#     and of plan/tools/browser-win32/js/gc-parse.js (the parser's setjmp and longjmp between collections of the
#     heap), --dump=layout of the bundled start.html, about.html and text.html;
#  M1 OmniGrammaton.exe draws start.html with Vulkan (lavapipe through Wine's Vulkan) in Xvfb (1280x800);
#  M2 the CPU's way: with --no-gpu (vulkan-1.dll is an ordinary import, so the program does not start
#     without it, and that is not tried);
#     a second start hands its URL to the first (WM_COPYDATA) and ends with 0; the window's right edge
#     dragged (xdotool) and the maximize button draw frames at the new sizes;
#  M3 https from the host's `openssl s_server` on 127.0.0.1 (a test CA made here, the network outside is
#     not used): refused while nothing trusts the CA, opened when the CA is in the current user's ROOT
#     store, refused when it is also in the Disallowed store;
#  M4 the location's box: about.html typed and Enter opens it, Back returns, the box's file: URL to the
#     clipboard (Ctrl+L, Ctrl+C) and typed back opens it again (Japanese with an input method: a real
#     Windows);
#  M5 letters typed into a page's text field reach it (the page prints its input events); the input
#     method's composition: a real Windows; M5t: plan/tools/browser-win32/tsf-main.c drives libkeiland's TSF
#     text store in a window of its own (the checks Wine's TSF can do: inserted text is committed, and
#     not handed on while the program does not ask for text);
#  M6 after the program is closed (its close button) and started again, a cookie with an expiry and
#     localStorage are still there, and a cookie without one is not; the cookies' records on disk are
#     protected for the user (DPAPI: each starts with the wrapping's magic), and
#     plan/tools/browser-win32/protect-main.c checks the wrapping backend over a files backend of its own;
#  caption: the bare title bar beside the location's box is the window's caption: the frame answers
#     WM_NCHITTEST there with HTCAPTION (plan/tools/browser-win32/hit-test-main.c sends it), on the box and the
#     buttons with HTCLIENT; a drag from there moves the window, a double click maximizes it and a second
#     restores it (told to the frame as Windows does: Wine does not move a maximized frame's X window
#     without a window manager; the snap of the drag and the system menu of a right click: a real
#     Windows);
#  M7 the dark mode (AppsUseLightTheme 0 in the registry) gives a dark title bar and the page its dark
#     scheme (start.html's prefers-color-scheme: dark, a dark ground; the light one in the light mode);
#     the mode changed while the program runs (plan/tools/browser-win32/theme-main.c writes the value and
#     broadcasts WM_SETTINGCHANGE "ImmersiveColorSet") turns the page light and dark again; at 144 dpi
#     the title bar is 1.5 times as tall; the snap layouts and Mica: a real Windows (Wine shows none).
# A window's screenshot passes when it has more than 100 colours (a page, not an empty window).  The PNGs
# go to build/review/browser-win32-wine/.  Each Wine command runs under flock /tmp/zedbsd-wine.lock (do not run
# the whole script under that lock: it would wait for itself).
#   sh plan/tools/browser-win32/wine-run.sh
# LLVM_MINGW=<root> chooses llvm-mingw (default $HOME/opt/llvm-mingw-20261020-ucrt-ubuntu-22.04-x86_64),
# PORT=<n> the https server's port (18444).  The Wine prefixes are build/tmp/browser-win32-wine (the console
# programs), build/tmp/browser-win32-wine-gui (the windows), build/tmp/browser-win32-wine-gui144 (LogPixels 144) and
# build/tmp/browser-win32-wine-dark (the dark mode); a missing one is made.  Last line: wine-run: PASS or
# wine-run: FAIL.
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
set -u
cd "$(dirname -- "$0")/../../.."
root=$(pwd)
. plan/tools/fresh-out.sh
out=build/tmp/browser-win32-wine-run
fresh_out "$out"
out=$root/$out
LLVM_MINGW=${LLVM_MINGW:-$HOME/opt/llvm-mingw-20261020-ucrt-ubuntu-22.04-x86_64}
port=${PORT:-18444}
build=build/tmp/browser-win32-wine-build/browser-win64
sources=build/tmp/browser-win32-sources
review=build/review/browser-win32-wine
lock=/tmp/zedbsd-wine.lock
lvp=/usr/share/vulkan/icd.d/lvp_icd.json
mkdir -p "$sources" "$review"
status=0
passed=0
failed=0
unrun=0

# The result of an item: result PASS|FAIL <item> <text>.
result() {
	verdict=$1
	item=$2
	shift 2
	printf 'wine-run: %s: %s: %s\n' "$item" "$verdict" "$*"
	if [ "$verdict" = PASS ]; then
		passed=$((passed + 1))
	else
		failed=$((failed + 1))
		status=1
	fi
}

# An item Wine cannot show.
unrun() {
	printf 'wine-run: %s: not run (a real Windows): %s\n' "$1" "$2"
	unrun=$((unrun + 1))
}

# 0. The builds.
win32_make() {
	log=$1
	shift
	timeout 3000 make --no-print-directory -f apps/browser-win32/Makefile WIN32_ARCH=x86_64 -j4 \
	    LLVM_MINGW="$LLVM_MINGW" BROWSER_WIN32_BUILD="$build" BROWSER_WIN32_SOURCES="$sources" "$@" > "$log" 2>&1 &&
	    ! grep -q 'warning:' "$log"
}
if ! win32_make "$out/make.log" all zip; then
	echo "wine-run: the Windows build did not build cleanly (log $out/make.log)"
	tail -10 "$out/make.log"
	echo "wine-run: FAIL"
	exit 1
fi
if ! win32_make "$out/make-cli.log" test-program TEST_PROGRAM=browser-cli \
    TEST_PROGRAM_SOURCES=plan/tools/browser-win32/browser-cli-main.c TEST_PROGRAM_LDFLAGS=-municode ||
    ! win32_make "$out/make-cert.log" test-program TEST_PROGRAM=cert-store \
    TEST_PROGRAM_SOURCES=plan/tools/browser-win32/cert-store-main.c ||
    ! win32_make "$out/make-theme.log" test-program TEST_PROGRAM=theme-set \
    TEST_PROGRAM_SOURCES=plan/tools/browser-win32/theme-main.c ||
    ! win32_make "$out/make-protect.log" test-program TEST_PROGRAM=protect-test \
    TEST_PROGRAM_SOURCES=plan/tools/browser-win32/protect-main.c ||
    ! win32_make "$out/make-hit.log" test-program TEST_PROGRAM=hit-test \
    TEST_PROGRAM_SOURCES=plan/tools/browser-win32/hit-test-main.c ||
    ! win32_make "$out/make-tsf.log" test-program TEST_PROGRAM=tsf-test \
    TEST_PROGRAM_SOURCES=plan/tools/browser-win32/tsf-main.c; then
	echo "wine-run: a test's program did not build cleanly (logs $out/make-cli.log, $out/make-cert.log, $out/make-theme.log, $out/make-protect.log, $out/make-hit.log, $out/make-tsf.log)"
	echo "wine-run: FAIL"
	exit 1
fi
if ! timeout 3600 sh plan/ws074/tests/host-build.sh plain > "$out/host-build.log" 2>&1; then
	echo "wine-run: the host's browser did not build (log $out/host-build.log)"
	echo "wine-run: FAIL"
	exit 1
fi
cli=$build/tests/browser-cli.exe
certs=$root/$build/tests/cert-store.exe
THEME_SET=$root/$build/tests/theme-set.exe
export THEME_SET
HIT_TEST=$root/$build/tests/hit-test.exe
export HIT_TEST
host=build/ws074-host/plain/browser
sync

# The zip, extracted into a new directory.
zip=$(ls "$build"/OmniGrammaton-*-win64.zip | head -1)
mkdir -p "$out/extracted"
if ! unzip -q "$zip" -d "$out/extracted"; then
	echo "wine-run: cannot extract $zip"
	echo "wine-run: FAIL"
	exit 1
fi
dist=$out/extracted/OmniGrammaton
echo "wine-run: $(basename "$zip") ($(wc -c < "$zip") bytes) extracted to $dist"

# The prefixes, each made once (so that making one prints nothing into a test's output), and the
# registry values the dpi's and the dark mode's prefixes need (set again on each run, the same value).
prefix() {
	if [ ! -f "$1/system.reg" ]; then
		WINEPREFIX=$1 WINEDEBUG=-all timeout 600 flock "$lock" xvfb-run -a wineboot -i > "$out/prefix-$(basename "$1").log" 2>&1
		WINEPREFIX=$1 timeout 120 wineserver -w
	fi
}
gui=$root/build/tmp/browser-win32-wine-gui
gui144=$root/build/tmp/browser-win32-wine-gui144
dark=$root/build/tmp/browser-win32-wine-dark
for each in "$root/build/tmp/browser-win32-wine" "$gui" "$gui144" "$dark"; do
	prefix "$each"
done
WINEPREFIX=$gui144 WINEDEBUG=-all timeout 300 flock "$lock" \
    wine reg add 'HKCU\Control Panel\Desktop' /v LogPixels /t REG_DWORD /d 144 /f > "$out/reg-144.log" 2>&1
WINEPREFIX=$dark WINEDEBUG=-all timeout 300 flock "$lock" \
    wine reg add 'HKCU\Software\Microsoft\Windows\CurrentVersion\Themes\Personalize' /v AppsUseLightTheme \
    /t REG_DWORD /d 0 /f > "$out/reg-dark.log" 2>&1
for each in "$gui144" "$dark"; do
	WINEPREFIX=$each timeout 120 wineserver -w
done

# M8. The headless modes, the same bytes on both.
fonts="--font=userland/desktop/fonts/Mahora-Regular.ttf --mono-font=userland/desktop/fonts/Mahora-Mono.ttf"
fonts="$fonts --fallback-font=userland/desktop/fonts/DroidSansFallbackFull.ttf"
same=0
differ=""
compare() {
	name=$1
	shift
	timeout 300 "$host" "$@" > "$out/$name.host" 2>&1
	WINEPREFIX=$root/build/tmp/browser-win32-wine WINEDEBUG=-all timeout 300 flock "$lock" wine "$cli" "$@" \
	    > "$out/$name.wine" 2>&1
	if cmp -s "$out/$name.host" "$out/$name.wine" && [ -s "$out/$name.host" ]; then
		same=$((same + 1))
	else
		differ="$differ $name"
	fi
}
total=0
for test in plan/ws074/tests/js/*.js plan/tools/browser-win32/js/*.js; do
	compare "js-$(basename "$test" .js)" --js "$test"
	total=$((total + 1))
done
for page in start about text; do
	# shellcheck disable=SC2086
	compare "dump-layout-$page" --dump=layout $fonts "userland/desktop/browser/data/$page.html"
	total=$((total + 1))
done
gc=$(tr -d '\r' < "$out/js-gc-parse.wine" 2> /dev/null | tr '\n' ';')
if [ -z "$differ" ]; then
	result PASS M8 "--js of $((total - 3)) files and --dump=layout of 3 pages: the same bytes on the host and under Wine ($same of $total; gc-parse under Wine: $gc)"
else
	result FAIL M8 "the host and Wine differ:$differ ($out/<name>.host, .wine)"
fi

# The windows, in Xvfb, one Xvfb for each start: windows.sh <the program's directory> <the output> <mode>
# <the program's options...>.  The mode: "second" (a second start, then a change of the size),
# "controls" (the title bar's controls), "input" (a page's text field), "close" (the close button,
# waiting for the program to end), "theme" (the apps' mode set light, then dark, while it runs, with
# $THEME_SET), "caption" (the bare title bar: the hit test with $HIT_TEST, a drag and double clicks),
# "wait" (waits for the start page's load to end) or "plain".
cat > "$out/windows.sh" << 'EOF'
#!/bin/sh
dist=$1
out=$2
mode=$3
shift 3
cd "$dist" || exit 2
# No key repeat: on a loaded host a key held between xdotool's press and release would repeat (a letter
# typed twice).
xset r off 2> /dev/null
WINEDEBUG=-all timeout 150 wine OmniGrammaton.exe "$@" > "$out/first.out" 2> "$out/first.err" &
program=$!
# Up to 90 seconds for the window, then the first frames.
for second in $(seq 90); do
	grep -q '^ZBROWSER READY' "$out/first.out" 2> /dev/null && break
	kill -0 "$program" 2> /dev/null || break
	sleep 1
done
echo "ready after $second s" > "$out/notes.txt"
if [ "$mode" = wait ] || [ "$mode" = close ]; then
	for second in $(seq 30); do
		grep -q '^ZBROWSER \(STOPPED\|ERROR load\)' "$out/first.out" 2> /dev/null && break
		sleep 1
	done
	echo "the load ended after $second s" >> "$out/notes.txt"
fi
sleep 5
import -window root "$out/first.png"
# The program's window: the largest of its class (Wine has small hidden windows of the same class).
window=
X=0
Y=0
WIDTH=0
HEIGHT=0
for each in $(xdotool search --class omnigrammaton 2> /dev/null); do
	geometry=$(xdotool getwindowgeometry --shell "$each" 2> /dev/null)
	size=$(echo "$geometry" | sed -n 's/^WIDTH=//p')
	if [ -n "$size" ] && [ "$size" -gt "$WIDTH" ]; then
		eval "$geometry"
		window=$each
	fi
done
echo "window $window x $X y $Y width $WIDTH height $HEIGHT" >> "$out/notes.txt"
if [ "$mode" = second ]; then
	WINEDEBUG=-all timeout 60 wine OmniGrammaton.exe share/browser/about.html > "$out/second.out" 2> "$out/second.err"
	echo "$?" > "$out/second.status"
	sleep 10
	import -window root "$out/second.png"
	# A new size: the right edge (inside its 8 pixels, the frame's hit test) dragged 126 pixels left,
	# which the window's own size loop follows (Wine without a window manager does not pass on a size
	# set from outside, xdotool windowsize); then the maximize button, the second from the right.
	xdotool mousemove $((X + WIDTH - 3)) $((Y + 400))
	sleep 1
	xdotool mousedown 1
	sleep 1
	for step in 1 2 3 4 5 6; do
		xdotool mousemove_relative -- -21 0
		sleep 0.3
	done
	xdotool mouseup 1
	sleep 6
	import -window root "$out/resized.png"
	tr -d '\r' < "$out/first.out" | grep '^ZBROWSER FRAME' | tail -1 > "$out/resized.txt"
	eval "$(xdotool getwindowgeometry --shell "$window")"
	xdotool mousemove $((X + WIDTH - 69)) $((Y + 16)) click 1
	sleep 6
	import -window root "$out/maximized.png"
	tr -d '\r' < "$out/first.out" | grep '^ZBROWSER FRAME' | tail -1 > "$out/maximized.txt"
fi
if [ "$mode" = controls ]; then
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
if [ "$mode" = input ]; then
	# The page's field (20 to 620 across, 20 to 60 down in the page, below the 32 pixels of the title bar).
	xdotool mousemove $((X + 200)) $((Y + 32 + 40)) click 1
	sleep 2
	xdotool type --delay 80 'hello wine'
	sleep 4
	import -window root "$out/input.png"
fi
if [ "$mode" = theme ]; then
	# The light mode, then the dark one again, each told to the windows as the Settings do.
	WINEDEBUG=-all timeout 60 wine "$THEME_SET" light > "$out/theme-light.out" 2>&1
	sleep 8
	import -window root "$out/theme-light.png"
	WINEDEBUG=-all timeout 60 wine "$THEME_SET" dark > "$out/theme-dark.out" 2>&1
	sleep 8
	import -window root "$out/theme-dark.png"
fi
if [ "$mode" = caption ]; then
	# The frame's answers (from the left edge, or the right when negative): the bare title bar left of
	# the caption buttons (the right 3 x 46 pixels and 10 more), below the box, the box, Back, Reload and
	# the minimize button.
	WINEDEBUG=-all timeout 60 wine "$HIT_TEST" -180 16 400 30 400 16 25 16 93 16 -115 16 > "$out/hit.out" 2>&1
	# A drag from the bare title bar, 100 pixels left and 50 down.
	echo "before $(xdotool getwindowgeometry --shell "$window" | tr '\n' ' ')" > "$out/moved.txt"
	xdotool mousemove $((X + WIDTH - 180)) $((Y + 16))
	sleep 1
	xdotool mousedown 1
	sleep 1
	for step in 1 2 3 4 5; do
		xdotool mousemove_relative -- -20 10
		sleep 0.3
	done
	xdotool mouseup 1
	sleep 3
	echo "after $(xdotool getwindowgeometry --shell "$window" | tr '\n' ' ')" >> "$out/moved.txt"
	# A double click there maximizes the frame, and one on the maximized frame's bare title bar restores
	# it.  Wine without a window manager does not move the X window of a maximized frame, so the second
	# double click is the one Windows tells the frame (hit-test --double: WM_NCLBUTTONDBLCLK with the
	# frame's own answer at the point).
	eval "$(xdotool getwindowgeometry --shell "$window")"
	xdotool mousemove $((X + WIDTH - 180)) $((Y + 16))
	sleep 1
	xdotool click --repeat 2 --delay 120 1
	sleep 5
	WINEDEBUG=-all timeout 60 wine "$HIT_TEST" -180 16 > "$out/hit-maximized.out" 2>&1
	WINEDEBUG=-all timeout 60 wine "$HIT_TEST" --double -180 16 > "$out/hit-restored.out" 2>&1
fi
if [ "$mode" = close ]; then
	# The close button, the rightmost 46 pixels of the title bar; then up to 40 seconds for the end.
	xdotool mousemove $((X + WIDTH - 23)) $((Y + 16)) click 1
	for second in $(seq 40); do
		kill -0 "$program" 2> /dev/null || break
		sleep 1
	done
	if kill -0 "$program" 2> /dev/null; then
		echo "running" > "$out/close.status"
	else
		wait "$program"
		echo "$?" > "$out/close.status"
	fi
	echo "closed after $second s" >> "$out/notes.txt"
fi
wineserver -k 2> /dev/null
wait
EOF

# One start in its own Xvfb: start <run> <prefix> <mode> <the program's options...>; the output in
# $out/<run>/ (first.txt the standard output without the CRs); $overrides is WINEDLLOVERRIDES (none when empty).
overrides=
start() {
	run=$1
	prefix=$2
	mode=$3
	shift 3
	mkdir -p "$out/$run"
	WINEPREFIX=$prefix VK_ICD_FILENAMES=$lvp WINEDLLOVERRIDES=$overrides timeout 600 flock "$lock" xvfb-run -a -s "-screen 0 1280x800x24" \
	    sh "$out/windows.sh" "$dist" "$out/$run" "$mode" "$@" > "$out/$run/xvfb.log" 2>&1
	tr -d '\r' < "$out/$run/first.out" > "$out/$run/first.txt" 2> /dev/null
	tr -d '\r' < "$out/$run/first.err" > "$out/$run/first-err.txt" 2> /dev/null
}
colours() {
	convert "$1" -format %k info: 2> /dev/null || echo 0
}
pixel() {
	convert "$1" -format "%[pixel:p{$2,$3}]" info: 2> /dev/null
}
# The brightness (0 to 255) of a pixel.
brightness() {
	convert "$1" -crop "1x1+$2+$3" -colorspace Gray -format '%[fx:int(255*u)]' info: 2> /dev/null || echo -1
}
# A window that drew its page: more than 100 colours and the READY line.
drew() {
	[ "$(colours "$out/$1/first.png")" -gt 100 ] && grep -q '^ZBROWSER READY' "$out/$1/first.txt"
}

# M1. Vulkan (with the second start and the new size).
start gpu "$gui" second share/browser/start.html
cp "$out/gpu/first.png" "$review/m1-gpu.png" 2> /dev/null
if drew gpu && ! grep -q 'drawing with the CPU' "$out/gpu/first-err.txt"; then
	result PASS M1 "start.html drawn with Vulkan ($(colours "$out/gpu/first.png") colours, $review/m1-gpu.png)"
else
	result FAIL M1 "no page drawn with Vulkan ($out/gpu/first.txt, first-err.txt)"
fi

# M2. The CPU's way: --no-gpu.
start nogpu "$gui" plain --no-gpu share/browser/start.html
cp "$out/nogpu/first.png" "$review/m2-nogpu.png" 2> /dev/null
if drew nogpu; then
	result PASS M2a "--no-gpu drew start.html ($(colours "$out/nogpu/first.png") colours, $review/m2-nogpu.png)"
else
	result FAIL M2a "--no-gpu drew no page ($out/nogpu/first.txt)"
fi

# The second start.
second=$(cat "$out/gpu/second.status" 2> /dev/null)
cp "$out/gpu/second.png" "$review/second.png" 2> /dev/null
if [ "$second" = 0 ] && grep -q '^ZBROWSER OPEN handed=.*about\.html$' "$out/gpu/first.txt" &&
    [ "$(colours "$out/gpu/second.png")" -gt 100 ]; then
	result PASS second "handed over and ended with 0; $(grep -m1 '^ZBROWSER OPEN' "$out/gpu/first.txt") ($review/second.png)"
else
	result FAIL second "status '$second', the first did not say it opened about.html ($out/gpu/first.txt)"
fi

# The new size: a narrower frame after the drag of the edge, the screen's width after the maximize button.
cp "$out/gpu/resized.png" "$review/resized.png" 2> /dev/null
cp "$out/gpu/maximized.png" "$review/maximized.png" 2> /dev/null
dragged=$(sed -n 's/.* width=\([0-9]*\) .*/\1/p' "$out/gpu/resized.txt" 2> /dev/null)
maximized=$(sed -n 's/.* width=\([0-9]*\) .*/\1/p' "$out/gpu/maximized.txt" 2> /dev/null)
if [ -n "$dragged" ] && [ "$dragged" -lt 1000 ] && [ "$dragged" -gt 850 ] && [ "$maximized" = 1280 ]; then
	result PASS resize "the right edge dragged: a frame $dragged wide; maximized: $(cat "$out/gpu/maximized.txt") ($review/resized.png, maximized.png)"
else
	result FAIL resize "the frame after the drag '$(cat "$out/gpu/resized.txt" 2> /dev/null)', after the maximize button '$(cat "$out/gpu/maximized.txt" 2> /dev/null)'"
fi

# M3. https: the server, a test CA and a certificate for 127.0.0.1 signed by it; the pages are whole
# HTTP responses (s_server -HTTP), the cookies' pages of M6 among them.
pki=$out/pki
site=$out/site
mkdir -p "$pki" "$site"
openssl req -x509 -newkey rsa:2048 -nodes -days 2 -subj "/CN=wine-run test CA" \
    -addext "basicConstraints=critical,CA:TRUE" -addext "keyUsage=critical,keyCertSign,cRLSign" \
    -keyout "$pki/ca.key" -out "$pki/ca.pem" > "$out/pki.log" 2>&1
openssl req -newkey rsa:2048 -nodes -subj "/CN=127.0.0.1" -keyout "$pki/server.key" -out "$pki/server.csr" >> "$out/pki.log" 2>&1
printf 'subjectAltName=IP:127.0.0.1\nbasicConstraints=CA:FALSE\nextendedKeyUsage=serverAuth\n' > "$pki/server.ext"
openssl x509 -req -in "$pki/server.csr" -CA "$pki/ca.pem" -CAkey "$pki/ca.key" -CAcreateserial -days 2 \
    -extfile "$pki/server.ext" -out "$pki/server.pem" >> "$out/pki.log" 2>&1
openssl x509 -in "$pki/ca.pem" -outform DER -out "$pki/ca.der" >> "$out/pki.log" 2>&1
token=$(date +%s)
crlf=$(printf '\r')
# response <file> <extra header lines, each ending with CR LF> <title> <script>
response() {
	body="<!DOCTYPE html><html><head><meta charset=\"utf-8\"><title>$3</title></head><body style=\"font-size:28px\"><p id=\"shown\">$3</p><script>$4</script></body></html>"
	{
		printf 'HTTP/1.1 200 OK\r\nContent-Type: text/html; charset=utf-8\r\nContent-Length: %s\r\nConnection: close\r\n' "$(printf '%s' "$body" | wc -c)"
		printf '%s' "$2"
		printf '\r\n%s' "$body"
	} > "$site/$1"
}
response page.html "" "https page" 'console.log("M3 page");'
show='var text = "cookie=" + document.cookie + " ls=" + localStorage.getItem("kept"); document.getElementById("shown").textContent = text;'
response set.html "Set-Cookie: kept=$token; Max-Age=86400; Path=/$crlf
Set-Cookie: session=$token; Path=/$crlf
" "set" "console.log(\"M6 before ls=\" + localStorage.getItem(\"kept\")); localStorage.setItem(\"kept\", \"$token\"); $show console.log(\"M6 set \" + text);"
response check.html "" "check" "$show console.log(\"M6 check \" + text);"
(cd "$site" && exec timeout 1800 openssl s_server -accept "127.0.0.1:$port" -cert ../pki/server.pem -key ../pki/server.key \
    -HTTP -quiet > ../server.log 2>&1) &
server=$!
for i in 1 2 3 4 5 6 7 8 9 10; do
	if /usr/bin/python3 -I -c "import socket,sys; s=socket.socket(); s.settimeout(1); sys.exit(s.connect_ex(('127.0.0.1', $port)))"; then
		break
	fi
	sleep 1
done
https=https://127.0.0.1:$port
# cert_store add|remove <store>
cert_store() {
	WINEPREFIX=$gui WINEDEBUG=-all timeout 120 flock "$lock" wine "$certs" "$1" "$2" "$pki/ca.der" 2>&1 | tr -d '\r' >> "$out/cert-store.txt"
	WINEPREFIX=$gui timeout 120 wineserver -w
}
cert_store remove Disallowed
cert_store remove Root
start untrusted "$gui" wait "$https/page.html"
cert_store add Root
start trusted "$gui" wait "$https/page.html"
cp "$out/trusted/first.png" "$review/m3-https.png" 2> /dev/null
cp "$out/untrusted/first.png" "$review/m3-untrusted.png" 2> /dev/null
if grep -q "^ZBROWSER ERROR load path=$https/page.html .*tls=.*verif" "$out/untrusted/first.txt"; then
	result PASS M3a "refused while nothing trusts the CA: $(grep -m1 '^ZBROWSER ERROR load' "$out/untrusted/first.txt" | cut -c1-160) ($review/m3-untrusted.png)"
else
	result FAIL M3a "not refused for the verification ($out/untrusted/first.txt)"
fi
if grep -q "^ZBROWSER NAVIGATE path=$https/page.html title=https page" "$out/trusted/first.txt" &&
    grep -q '^ZBROWSER CONSOLE level=[0-9]* M3 page' "$out/trusted/first.txt" && drew trusted; then
	result PASS M3b "opened with the CA in the ROOT store ($review/m3-https.png)"
else
	result FAIL M3b "the page did not open with the CA in the ROOT store ($out/trusted/first.txt)"
fi

# M6. The cookies and localStorage, set, the program closed, started again.
start set "$gui" close "$https/set.html"
start check "$gui" close "$https/check.html"
cp "$out/check/first.png" "$review/m6-after-restart.png" 2> /dev/null
before=$(grep -m1 '^ZBROWSER CONSOLE level=[0-9]* M6 before' "$out/set/first.txt" | sed 's/.* M6 before //')
set_line=$(grep -m1 '^ZBROWSER CONSOLE level=[0-9]* M6 set' "$out/set/first.txt" | sed 's/.* M6 set //')
check_line=$(grep -m1 '^ZBROWSER CONSOLE level=[0-9]* M6 check' "$out/check/first.txt" | sed 's/.* M6 check //')
closes="$(cat "$out/set/close.status" 2> /dev/null) $(cat "$out/check/close.status" 2> /dev/null)"
if [ "$before" != "ls=$token" ] && echo "$set_line" | grep -q "kept=$token" &&
    echo "$check_line" | grep -q "kept=$token" && echo "$check_line" | grep -q "ls=$token\$" &&
    ! echo "$check_line" | grep -q "session=" && [ "$closes" = "0 0" ]; then
	result PASS M6 "set: '$set_line'; after the close button (exit $closes) and a new start: '$check_line' ($review/m6-after-restart.png)"
else
	result FAIL M6 "before '$before', set '$set_line', after the restart '$check_line', the closes' exit '$closes'"
fi
# The cookies' records of the program's data directory, and the wrapping backend's own test.
records=0
plain=0
for record in "$gui"/drive_c/users/*/AppData/Local/OmniGrammaton/cookies/*; do
	[ -f "$record" ] || continue
	records=$((records + 1))
	[ "$(head -c 8 "$record")" = OGDPAPI1 ] || plain=$((plain + 1))
done
mkdir -p "$out/protect"
WINEPREFIX=$root/build/tmp/browser-win32-wine WINEDEBUG=-all timeout 300 flock "$lock" wine "$root/$build/tests/protect-test.exe" \
    "Z:$(echo "$out/protect" | tr / '\\')" > "$out/protect.log" 2>&1
protect=$(tr -d '\r' < "$out/protect.log" | tail -1)
if [ "$records" -gt 0 ] && [ "$plain" = 0 ] && echo "$protect" | grep -q '^test-protect: PASS'; then
	result PASS M6p "the cookies' $records records on disk are protected (DPAPI); $protect"
else
	result FAIL M6p "records $records, of them not protected $plain; $protect ($out/protect.log)"
fi

# M3. The CA also in the Disallowed store: refused.
cert_store add Disallowed
start disallowed "$gui" wait "$https/page.html"
cert_store remove Disallowed
cert_store remove Root
cp "$out/disallowed/first.png" "$review/m3-disallowed.png" 2> /dev/null
if grep -q "^ZBROWSER ERROR load path=$https/page.html " "$out/disallowed/first.txt" &&
    ! grep -q "^ZBROWSER NAVIGATE path=$https/page.html title=https page" "$out/disallowed/first.txt"; then
	result PASS M3c "refused with the CA also in the Disallowed store: $(grep -m1 '^ZBROWSER ERROR load' "$out/disallowed/first.txt" | cut -c1-160) ($review/m3-disallowed.png)"
else
	result FAIL M3c "not refused with the CA in the Disallowed store ($out/disallowed/first.txt)"
fi
kill "$server" 2> /dev/null
wait "$server" 2> /dev/null
unrun M3d "a public site with the roots of Windows (the network outside is not used)"

# M4. The title bar's controls and the location's box.
start controls "$gui" controls share/browser/start.html
convert "$out/controls/first.png" -crop 1280x32+0+0 "$review/titlebar.png" 2> /dev/null
for each in typing typed back retyped; do
	cp "$out/controls/$each.png" "$review/m4-$each.png" 2> /dev/null
done
navigations=$(grep '^ZBROWSER NAVIGATE path=' "$out/controls/first.txt" | sed 's/ title=.*//; s/.*[\\/]//')
typed=$(echo "$navigations" | sed -n 2p)
back=$(echo "$navigations" | sed -n 3p)
retyped=$(echo "$navigations" | sed -n 4p)
if [ "$typed" = about.html ] && [ "$back" = start.html ] && [ "$retyped" = about.html ]; then
	result PASS M4 "about.html typed opened, Back returned to start.html, the box's file: URL typed back opened about.html ($review/m4-typed.png, m4-back.png, m4-retyped.png)"
else
	result FAIL M4 "the pages opened were: $(echo $navigations) ($out/controls/first.txt)"
fi
if grep -q '^file:///[A-Za-z]:/.*about\.html' "$out/controls/clipboard.txt" 2> /dev/null; then
	result PASS clipboard "the box's text copied: $(head -c 200 "$out/controls/clipboard.txt" | tr -d '\r')"
else
	result FAIL clipboard "the box's text is not a file: URL of the drive's path on the clipboard ($out/controls/clipboard.txt)"
fi
unrun M4j "Japanese with an input method (MS-IME) in the box, the candidates' window"

# M5. A page's text field.
start input "$gui" input "$root/plan/tools/browser-win32/wine-pages/input.html"
cp "$out/input/input.png" "$review/m5-input.png" 2> /dev/null
value=$(grep '^ZBROWSER CONSOLE level=[0-9]* M5 value=' "$out/input/first.txt" | tail -1 | sed 's/.* M5 value=//')
if [ "$value" = "hello wine" ]; then
	result PASS M5 "the field took 'hello wine' ($(grep -c ' M5 value=' "$out/input/first.txt") input events, $review/m5-input.png)"
else
	result FAIL M5 "the field's last value '$value' ($out/input/first.txt)"
fi
unrun M5j "an input method's composition in the page's field"

# M5t. libkeiland's TSF text store, driven in its own process as an input method would (Wine's TSF has
# no compositions and no view: those of its checks are not run).
WINEPREFIX=$gui WINEDEBUG=-all timeout 300 flock "$lock" xvfb-run -a wine "$root/$build/tests/tsf-test.exe" \
    > "$out/tsf.log" 2>&1
WINEPREFIX=$gui timeout 120 wineserver -w
tsf=$(tr -d '\r' < "$out/tsf.log" | grep '^tsf-test: ' | tail -1)
if echo "$tsf" | grep -q '^tsf-test: PASS'; then
	result PASS M5t "the text store hands inserted text on as a commit, and none while text is not wanted; $tsf"
else
	result FAIL M5t "$tsf ($out/tsf.log)"
fi
unrun M5c "TSF's composition shown, committed and dropped, and the candidates at the caret (Wine's TSF has no compositions)"
unrun M5d "a precision touch pad's scrolling through DirectManipulation and its fling after the fingers lift"

# The bare title bar as the caption: the hit test, the drag, the double clicks.
start caption "$gui" caption share/browser/start.html
convert "$out/caption/first.png" -crop 1280x32+0+0 "$review/titlebar-caption.png" 2> /dev/null
tr -d '\r' < "$out/caption/hit.out" > "$out/caption/hit.txt" 2> /dev/null
answers=$(sed -n 's/^hit-test: -\{0,1\}[0-9]* [0-9]* \([0-9]*\)$/\1/p' "$out/caption/hit.txt" | tr '\n' ' ')
if [ "$answers" = "2 2 1 1 1 8 " ]; then
	result PASS caption-hit "HTCAPTION on the bare title bar and below the box, HTCLIENT on the box, Back and Reload, HTMINBUTTON: $(echo $answers) ($review/titlebar-caption.png)"
else
	result FAIL caption-hit "the answers were '$answers', not '2 2 1 1 1 8' ($out/caption/hit.txt)"
fi
before_x=$(sed -n 's/^before .*X=\(-\{0,1\}[0-9]*\) .*/\1/p' "$out/caption/moved.txt" 2> /dev/null)
before_y=$(sed -n 's/^before .*Y=\(-\{0,1\}[0-9]*\) .*/\1/p' "$out/caption/moved.txt" 2> /dev/null)
after_x=$(sed -n 's/^after .*X=\(-\{0,1\}[0-9]*\) .*/\1/p' "$out/caption/moved.txt" 2> /dev/null)
after_y=$(sed -n 's/^after .*Y=\(-\{0,1\}[0-9]*\) .*/\1/p' "$out/caption/moved.txt" 2> /dev/null)
if [ -n "$before_x" ] && [ -n "$after_x" ] && [ $((before_x - after_x)) -ge 80 ] && [ $((after_y - before_y)) -ge 40 ]; then
	result PASS caption-drag "a drag from the bare title bar moved the window from $before_x,$before_y to $after_x,$after_y"
else
	result FAIL caption-drag "the window at '$before_x,$before_y' before the drag and '$after_x,$after_y' after ($out/caption/moved.txt)"
fi
maximized=$(tr -d '\r' < "$out/caption/hit-maximized.out" 2> /dev/null | sed -n 's/^hit-test: frame \([0-9]*\) .*/\1/p')
bare=$(tr -d '\r' < "$out/caption/hit-maximized.out" 2> /dev/null | sed -n 's/^hit-test: [0-9]* [0-9]* \([0-9]*\)$/\1/p')
restored=$(tr -d '\r' < "$out/caption/hit-restored.out" 2> /dev/null | sed -n 's/^hit-test: frame \([0-9]*\) .*/\1/p' | tail -1)
if [ "$maximized" = 1280 ] && [ "$bare" = 2 ] && [ -n "$restored" ] && [ "$restored" -lt 1280 ]; then
	result PASS caption-double "a double click on the bare title bar maximized the frame ($maximized wide, HTCAPTION there too), a second restored it ($restored wide)"
else
	result FAIL caption-double "the frame was '$maximized' wide after the double click (answer '$bare' there) and '$restored' after the second ($out/caption/hit-maximized.out, hit-restored.out)"
fi
unrun caption-snap "Aero Snap by the drag from the bare title bar and the system menu of a right click on it"

# M7. The dark mode (the title bar and the page), changed while the program runs, and 144 dpi.
start dark "$dark" theme share/browser/start.html
cp "$out/dark/first.png" "$review/m7-dark.png" 2> /dev/null
cp "$out/dark/theme-light.png" "$review/m7-to-light.png" 2> /dev/null
cp "$out/dark/theme-dark.png" "$review/m7-to-dark.png" 2> /dev/null
convert "$out/dark/first.png" -crop 1280x32+0+0 "$review/titlebar-dark.png" 2> /dev/null
light_bar=$(brightness "$out/gpu/first.png" 700 5)
dark_bar=$(brightness "$out/dark/first.png" 700 5)
if drew dark && [ "$dark_bar" -ge 0 ] && [ "$dark_bar" -lt 100 ] && [ "$light_bar" -gt 150 ]; then
	result PASS M7 "the title bar's ground: brightness $dark_bar dark, $light_bar light ($review/titlebar-dark.png)"
else
	result FAIL M7 "the title bar's ground: brightness $dark_bar dark, $light_bar light ($out/dark/first.png)"
fi
# The page's ground beside start.html's column (x 950 of the 1024 pixels wide window), and the scheme the program gave the page.
light_page=$(brightness "$out/gpu/first.png" 950 600)
dark_page=$(brightness "$out/dark/first.png" 950 600)
light_scheme=$(grep -m1 '^ZBROWSER SCHEME' "$out/gpu/first.txt" | sed 's/^ZBROWSER SCHEME //')
dark_scheme=$(grep -m1 '^ZBROWSER SCHEME' "$out/dark/first.txt" | sed 's/^ZBROWSER SCHEME //')
if [ "$dark_page" -ge 0 ] && [ "$dark_page" -lt 100 ] && [ "$light_page" -gt 150 ] &&
    [ "$light_scheme" = dark=0 ] && [ "$dark_scheme" = dark=1 ]; then
	result PASS M7p "the page's ground in the dark mode: brightness $dark_page dark, $light_page light (prefers-color-scheme, $review/m7-dark.png)"
else
	result FAIL M7p "the page's ground: brightness $dark_page dark, $light_page light, the first SCHEME lines light '$light_scheme' dark '$dark_scheme' ($out/dark/first.txt)"
fi
# The mode changed while it runs: light, then dark again.
to_light=$(brightness "$out/dark/theme-light.png" 950 600)
to_dark=$(brightness "$out/dark/theme-dark.png" 950 600)
to_light_bar=$(brightness "$out/dark/theme-light.png" 700 5)
schemes=$(grep '^ZBROWSER SCHEME' "$out/dark/first.txt" | sed 's/^ZBROWSER SCHEME //' | tr '\n' ' ')
told=$(cat "$out/dark/theme-light.out" "$out/dark/theme-dark.out" 2> /dev/null | tr -d '\r' | tr '\n' ' ')
if [ "$to_light" -gt 150 ] && [ "$to_light_bar" -gt 150 ] && [ "$to_dark" -ge 0 ] && [ "$to_dark" -lt 100 ] &&
    [ "$schemes" = "dark=1 dark=0 dark=1 " ]; then
	result PASS M7t "the mode changed while it runs: the page $to_light and the title bar $to_light_bar light, the page $to_dark dark again (SCHEME $schemes; $told; $review/m7-to-light.png, m7-to-dark.png)"
else
	result FAIL M7t "after the changes: page $to_light, title bar $to_light_bar, page $to_dark, SCHEME $schemes ($told; $out/dark/)"
fi
unrun M7s "the snap layouts over the maximize button (Windows 11)"
unrun M7m "Mica behind the frame and through the title bar (Windows 11 22H2), the accent of the system (Wine has no DWM colours)"
start dpi144 "$gui144" plain share/browser/start.html
convert "$out/dpi144/first.png" -crop 1280x48+0+0 "$review/titlebar-144dpi.png" 2> /dev/null
cp "$out/dpi144/first.png" "$review/dpi144.png" 2> /dev/null
# The bare title bar left of the caption buttons (3 x 69 pixels and 15 more at 144 dpi), at y 5 and 40.
bare=$(sed -n 's/^window .* x \(-\{0,1\}[0-9]*\) y .* width \([0-9]*\) height .*/\1 \2/p' "$out/dpi144/notes.txt" 2> /dev/null)
bare=$(echo "$bare" | awk '{ print $1 + $2 - 240 }')
top=$(pixel "$out/dpi144/first.png" "$bare" 5)
low=$(pixel "$out/dpi144/first.png" "$bare" 40)
page=$(pixel "$out/gpu/first.png" 700 40)
if [ -n "$top" ] && [ "$top" = "$low" ] && [ "$page" != "$low" ]; then
	result PASS dpi "at 144 dpi the title bar is 48 pixels tall ($review/titlebar-144dpi.png)"
else
	result FAIL dpi "the title bar is not 1.5 times as tall (y 5 $top, y 40 $low, 96 dpi y 40 $page)"
fi

sync
echo "wine-run: $passed passed, $failed failed, $unrun not run (a real Windows); Wine is the reference, not the judgement of a real Windows"
if [ "$status" = 0 ]; then echo "wine-run: PASS"; else echo "wine-run: FAIL"; fi
exit "$status"

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
#     the first says so (ZBROWSER OPEN) and shows it: second.png.
# A screenshot passes when it has more than 100 colours (a page, not an empty window).  The PNGs are
# copied to build/review/ws334-p028/.  Wine runs under flock /tmp/zedbsd-wine.lock.
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

# 2 to 4. The windows, in Xvfb, one Xvfb for each start.
cat > "$out/windows.sh" << 'EOF'
#!/bin/sh
# In Xvfb: $1 the program's directory, $2 the output, $3 the program's options (one word or none), $4 "second"
# for a second start.
dist=$1
out=$2
options=$3
second=$4
cd "$dist" || exit 2
WINEDEBUG=-all timeout 90 wine OmniGrammaton.exe $options share/browser/start.html > "$out/first.out" 2> "$out/first.err" &
sleep 25
import -window root "$out/first.png"
if [ "$second" = second ]; then
	WINEDEBUG=-all timeout 60 wine OmniGrammaton.exe share/browser/about.html > "$out/second.out" 2> "$out/second.err"
	echo "$?" > "$out/second.status"
	sleep 10
	import -window root "$out/second.png"
fi
wineserver -k 2> /dev/null
wait
EOF
colours() {
	convert "$1" -format %k info: 2> /dev/null || echo 0
}
for run in gpu nogpu; do
	mkdir -p "$out/$run"
	options=
	second=
	[ "$run" = nogpu ] && options=--no-gpu
	[ "$run" = gpu ] && second=second
	WINEPREFIX=$root/build/tmp/ws334-wine-gui VK_ICD_FILENAMES=/usr/share/vulkan/icd.d/lvp_icd.json \
	    timeout 600 flock /tmp/zedbsd-wine.lock xvfb-run -a -s "-screen 0 1280x800x24" \
	    sh "$out/windows.sh" "$dist" "$out/$run" "$options" "$second" > "$out/$run/xvfb.log" 2>&1
	tr -d '\r' < "$out/$run/first.out" > "$out/$run/first.txt" 2> /dev/null
	count=$(colours "$out/$run/first.png")
	cp "$out/$run/first.png" "$review/$run.png" 2> /dev/null
	if [ "$count" -gt 100 ] && grep -q '^ZBROWSER READY' "$out/$run/first.txt"; then
		echo "wine-run: $run: the window drew start.html ($count colours, $review/$run.png)"
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
sync
if [ "$status" = 0 ]; then echo "wine-run: PASS"; else echo "wine-run: FAIL"; fi
exit "$status"

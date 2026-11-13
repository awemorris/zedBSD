#!/bin/sh
# libbrowser's tests (plan/ws074/tests) on Windows under Wine, next to the same tests on the host, with the
# differences listed (the reference, not the judgement of a real Windows).  Wine's result is for comparing:
# the pass line is that every difference has a reason (the table in the phase's result), so the last line
# is "host-tests-windows: done" with the number of differences, not PASS.
#  1. the Windows libraries (make -f apps/browser-win32/Makefile layers libbrowser-core libraries vulkan
#     headers, test-program browser-cli) and the host's build (plan/ws074/tests/host-build.sh plain);
#  2. every plan/ws074/tests/host-*.c as a Windows .exe (the same compile flags as the Windows build, linked
#     with every static library and plan/ws334/tests/binary-mode.c, which makes the standard streams and
#     files binary as on the host; win32-posix.h gives setenv and realpath; host-heap is compiled from a copy
#     whose 64-bit state is an unsigned long long, and with -fno-inline; host-svg-length and host-svg-text
#     from copies whose temporary directory is on Wine's Z: drive), run on the host and under Wine from the
#     repository's root; the exit status and the output (CR removed) are compared;
#  3. the runners that start a program, once with the host's program and once with the same program under
#     Wine (a small wrapper script that puts wine before it):
#     run-js-tests.py (--program browser-cli.exe), run-test262.py (--driver host-js.exe), run-html5lib-tree.py
#     (--driver host-tree.exe), run-url-tests.py (--driver host-url.exe); the summary lines and, for
#     test262, the list of failing tests are compared;
#  4. --render-gpu and --render of a few pages (gpu-compare.py, lavapipe on both sides): the pictures the host
#     and Wine drew are compared (compare-ppm.py), and the GPU's against the CPU's on each side;
#  5. Interop 2025's focus areas: plan/ws074/tests/run-wpt-reftests.py over the same 20 tests of each of five
#     areas' directories (the selection is the runner's stratified one for --limit 20), on both sides; the
#     outcome of each test is compared.
# The parts to run: ONLY="build host runners gpu interop" (default: all; runners is js url html5lib
# test262, each also by its own name).  TEST262_FILTERS="test/built-ins/Math/ test/annexB/" chooses the test262
# directories (default: five; all: all of test262, which takes hours under Wine).
#   sh plan/ws334/tests/host-tests-windows.sh
# LLVM_MINGW=<root> chooses llvm-mingw (default $HOME/opt/llvm-mingw-20261020-ucrt-ubuntu-22.04-x86_64);
# the output is build/tmp/ws334-host-tests-windows (a new directory each run); the summary there is
# summary.txt, and the table of the host tests is host-table.txt.  Each Wine command runs under
# flock /tmp/zedbsd-wine.lock (do not run the whole script under that lock: it would wait for itself).  Wine's
# prefix is build/tmp/ws334-wine (made when missing).
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
set -u
cd "$(dirname -- "$0")/../../.."
root=$(pwd)
. plan/tools/fresh-out.sh
name=build/tmp/ws334-host-tests-windows
fresh_out "$name"
out=$(cd "$root/$name" && pwd -P)
LLVM_MINGW=${LLVM_MINGW:-$HOME/opt/llvm-mingw-20261020-ucrt-ubuntu-22.04-x86_64}
ONLY=${ONLY:-build host runners gpu interop}
build=build/tmp/ws334-wine-build/browser-win64
sources=build/tmp/ws334-win32-sources
lock=/tmp/zedbsd-wine.lock
lvp=/usr/share/vulkan/icd.d/lvp_icd.json
prefix=$root/build/tmp/ws334-wine
python=/usr/bin/python3
summary=$out/summary.txt
: > "$summary"
mkdir -p "$out/bin" "$out/exe" "$out/linux" "$out/wine" "$sources"
differences=0

say() {
	printf '%s\n' "$*" | tee -a "$summary"
}

wants() {
	case " $ONLY " in
	*" $1 "*) return 0 ;;
	esac
	return 1
}

win32_make() {
	log=$1
	shift
	timeout 3000 make --no-print-directory -f apps/browser-win32/Makefile WIN32_ARCH=x86_64 -j4 \
	    LLVM_MINGW="$LLVM_MINGW" BROWSER_WIN32_BUILD="$build" BROWSER_WIN32_SOURCES="$sources" "$@" > "$log" 2>&1 &&
	    ! grep -q 'warning:' "$log"
}

# The Wine prefix, made once.
if [ ! -f "$prefix/system.reg" ]; then
	WINEPREFIX=$prefix WINEDEBUG=-all timeout 600 flock "$lock" xvfb-run -a wineboot -i > "$out/prefix.log" 2>&1
	WINEPREFIX=$prefix timeout 120 wineserver -w
fi

# 1. The builds.
if wants build || wants host || wants runners || wants gpu || wants interop; then
	if ! win32_make "$out/make.log" layers libbrowser-core libraries vulkan headers; then
		say "host-tests-windows: the Windows libraries did not build cleanly (log $out/make.log)"
		tail -10 "$out/make.log"
		exit 1
	fi
	if ! win32_make "$out/make-cli.log" test-program TEST_PROGRAM=browser-cli \
	    TEST_PROGRAM_SOURCES=plan/ws334/tests/browser-cli-main.c TEST_PROGRAM_LDFLAGS=-municode; then
		say "host-tests-windows: browser-cli.exe did not build cleanly (log $out/make-cli.log)"
		exit 1
	fi
	if ! timeout 3600 sh plan/ws074/tests/host-build.sh plain > "$out/host-build.log" 2>&1; then
		say "host-tests-windows: the host's build failed (log $out/host-build.log)"
		exit 1
	fi
	sync
	say "host-tests-windows: built (host: $(grep -c '^built ' "$out/host-build.log") programs, warnings $(grep -c 'warning:' "$out/host-build.log"))"
fi
cli=$root/$build/tests/browser-cli.exe
hostdir=$root/build/ws074-host/plain

# The flags the build compiles and links with.
eval "$(make -s --no-print-directory -f apps/browser-win32/Makefile WIN32_ARCH=x86_64 LLVM_MINGW="$LLVM_MINGW" \
    BROWSER_WIN32_BUILD="$build" flags | sed "s/'/'\\\\''/g; s/^\([A-Z0-9_]*\)=\(.*\)\$/\1='\2'/")"
cflags=$(echo "$WIN32_CFLAGS" | sed 's/-MMD -MP//')

# Runs a Windows program under Wine: wine-exe <program> <arguments...> (the lock around each run).  A run over
# WINE_EXE_TIMEOUT seconds (default 3000) is ended with its Wine server: the Windows program is not the
# wrapper's child, so a plain timeout would leave it running and holding the lock.
cat > "$out/bin/wine-exe" << EOF
#!/bin/sh
export WINEPREFIX=$prefix WINEDEBUG=-all VK_ICD_FILENAMES=$lvp
exec flock $lock sh -c '
	timeout -k 3 \${WINE_EXE_TIMEOUT:-3000} wine "\$@"
	code=\$?
	if [ \$code -ge 124 ]; then
		wineserver -k
		wineserver -w
	fi
	exit \$code' sh "\$@"
EOF
chmod +x "$out/bin/wine-exe"

# A wrapper that runs one Windows program, for a runner's --program or --driver.
wrapper() {
	printf '#!/bin/sh\nWINE_EXE_TIMEOUT=100 exec %s %s "$@"\n' "$out/bin/wine-exe" "$1" > "$2"
	chmod +x "$2"
}

# 2. The host tests, each built as an .exe (also for the runners, which use three of them) and run on both sides.
if wants host || wants runners || wants js || wants url || wants html5lib || wants test262; then
	cat > "$out/compile.sh" << EOF
#!/bin/sh
# compile.sh <name>: plan/ws074/tests/<name>.c to $out/exe/<name>.exe
n=\$1
source=plan/ws074/tests/\$n.c
extra=
# host-heap keeps a 64-bit state in an unsigned long, which is 32 bits on Windows: a copy with unsigned long long.
# It also holds cells in main's own frame, which Win64's frame address (rbp points into the frame, not at its
# top) does not bound for the collector's stack scan once the compiler inlines the tests into main:
# -fno-inline keeps each test in a frame of its own below main's.
if [ "\$n" = host-heap ]; then
	extra=-fno-inline
	sed 's/unsigned long state/unsigned long long state/; s/\([0-9]\)UL\b/\1ULL/g' \$source > $out/exe/\$n.c
	source=$out/exe/\$n.c
fi
# host-svg-length and host-svg-text make their page's directory from the template "/tmp/...", an absolute path
# on POSIX but one relative to the current drive on Windows, which is no local page's location: a copy with
# Wine's Z: drive before it (the same directory).
if [ "\$n" = host-svg-length ] || [ "\$n" = host-svg-text ]; then
	sed 's|"/tmp/ws074-|"Z:/tmp/ws074-|' \$source > $out/exe/\$n.c
	source=$out/exe/\$n.c
fi
$WIN32_CC $cflags -include plan/ws334/tests/win32-posix.h \$extra -Iplan/ws074/tests -Iuserland/desktop/browser -c \$source -o $out/exe/\$n.o > $out/exe/\$n.log 2>&1 &&
    $WIN32_CC -static -o $out/exe/\$n.exe $out/exe/\$n.o $out/binary-mode.o $WIN32_PROGRAM_ARCHIVES $WIN32_PROGRAM_LDFLAGS >> $out/exe/\$n.log 2>&1
EOF
	$WIN32_CC $cflags -c plan/ws334/tests/binary-mode.c -o "$out/binary-mode.o" || exit 1
	ls plan/ws074/tests/host-*.c | sed 's|.*/||; s|\.c$||' | grep -vx host-shell > "$out/host-names.txt"
	xargs -P 16 -n 1 sh "$out/compile.sh" < "$out/host-names.txt"
	sync
fi
if wants host; then
	: > "$out/host-table.txt"
	compiled=0
	for each in $(cat "$out/host-names.txt"); do
		if [ ! -x "$out/exe/$each.exe" ]; then
			printf '%s: does not build on Windows (%s)\n' "$each" "$(grep -m1 'error' "$out/exe/$each.log")" >> "$out/host-table.txt"
			differences=$((differences + 1))
			continue
		fi
		compiled=$((compiled + 1))
		(cd "$root" && ASAN_OPTIONS=detect_stack_use_after_return=0 timeout 600 "$hostdir/$each" > "$out/linux/$each.log" 2>&1; echo $? > "$out/linux/$each.exit")
		(cd "$root" && timeout 600 "$out/bin/wine-exe" "$out/exe/$each.exe" > "$out/wine/$each.raw" 2>&1; echo $? > "$out/wine/$each.exit")
		tr -d '\r' < "$out/wine/$each.raw" > "$out/wine/$each.log"
		le=$(cat "$out/linux/$each.exit")
		we=$(cat "$out/wine/$each.exit")
		if [ "$le" = "$we" ] && cmp -s "$out/linux/$each.log" "$out/wine/$each.log"; then
			verdict=same
		elif [ "$le" = "$we" ]; then
			verdict="output differs"
			differences=$((differences + 1))
		else
			verdict="exit differs"
			differences=$((differences + 1))
		fi
		printf '%s: host exit %s, Wine exit %s, %s; host: %s; Wine: %s\n' "$each" "$le" "$we" "$verdict" \
		    "$(tail -1 "$out/linux/$each.log" | cut -c1-90)" "$(tail -1 "$out/wine/$each.log" | cut -c1-90)" >> "$out/host-table.txt"
	done
	sync
	say "host-tests: $(wc -l < "$out/host-names.txt") host-*.c, $compiled built as .exe, $(grep -c ', same;' "$out/host-table.txt") the same on both sides, $(grep -vc ', same;' "$out/host-table.txt") other (host-table.txt)"
fi

# 3. The runners (each part by its own name too: js url html5lib test262).
if wants runners || wants js || wants url || wants html5lib || wants test262; then
	wrapper "$cli" "$out/bin/browser-wine"
	for each in host-js host-tree host-url; do
		[ -x "$out/exe/$each.exe" ] && wrapper "$out/exe/$each.exe" "$out/bin/$each-wine"
	done
	if wants runners || wants js; then
		timeout 3600 $python plan/ws074/tests/run-js-tests.py --program "$hostdir/browser" > "$out/js.host" 2>&1
		timeout 3600 $python plan/ws074/tests/run-js-tests.py --program "$out/bin/browser-wine" > "$out/js.wine" 2>&1
		say "run-js-tests: host: $(tail -1 "$out/js.host"); Wine: $(tail -1 "$out/js.wine")"
		cmp -s "$out/js.host" "$out/js.wine" || differences=$((differences + 1))
	fi
	if wants runners || wants url; then
		timeout 3600 $python plan/ws074/tests/run-url-tests.py --driver "$hostdir/host-url" > "$out/url.host" 2>&1
		timeout 3600 $python plan/ws074/tests/run-url-tests.py --driver "$out/bin/host-url-wine" > "$out/url.wine" 2>&1
		say "run-url-tests: host: $(tr '\n' ' ' < "$out/url.host"); Wine: $(tr '\n' ' ' < "$out/url.wine")"
		cmp -s "$out/url.host" "$out/url.wine" || differences=$((differences + 1))
	fi
	if wants runners || wants html5lib; then
		timeout 3600 $python plan/ws074/tests/run-html5lib-tree.py --driver "$hostdir/host-tree" > "$out/html5lib.host" 2>&1
		timeout 3600 $python plan/ws074/tests/run-html5lib-tree.py --driver "$out/bin/host-tree-wine" > "$out/html5lib.wine" 2>&1
		say "run-html5lib-tree: host: $(head -1 "$out/html5lib.host"); Wine: $(head -1 "$out/html5lib.wine")"
		cmp -s "$out/html5lib.host" "$out/html5lib.wine" || differences=$((differences + 1))
	fi
	# The failing tests of both are compared, for each of TEST262_FILTERS (default: five directories, 2200 tests;
	# TEST262_FILTERS=all runs all of test262, which under Wine takes hours: a test that hangs ends a chunk of
	# 400 after 100 s and the tests after it are run one by one).
	if wants runners || wants test262; then
		filters=${TEST262_FILTERS:-"test/built-ins/Math/ test/built-ins/JSON/ test/built-ins/Array/prototype/map/ test/language/statements/for/ test/annexB/"}
		[ "$filters" = all ] && filters=
		for filter in ${filters:-all}; do
			tag=$(echo "$filter" | tr '/' '-')
			arguments=
			[ "$filter" != all ] && arguments="--filter $filter"
			# shellcheck disable=SC2086
			timeout 7200 $python plan/ws074/tests/run-test262.py --jobs 16 --failures "$out/test262$tag.host.failures" $arguments \
			    --driver "$hostdir/host-js" > "$out/test262$tag.host" 2>&1
			# (Wine runs one driver at a time, as the lock makes it: with --jobs 1 the runner's 10 s limit for a test does
			# not count the wait for the lock.)
			# shellcheck disable=SC2086
			timeout 7200 $python plan/ws074/tests/run-test262.py --jobs 1 --failures "$out/test262$tag.wine.failures" $arguments \
			    --driver "$out/bin/host-js-wine" > "$out/test262$tag.wine" 2>&1
			if cmp -s "$out/test262$tag.host.failures" "$out/test262$tag.wine.failures"; then
				same="the failing tests are the same ($(wc -l < "$out/test262$tag.host.failures"))"
			else
				differences=$((differences + 1))
				same="the failing tests differ: $(diff "$out/test262$tag.host.failures" "$out/test262$tag.wine.failures" | grep -c '^[<>]') lines (test262$tag.*.failures)"
			fi
			say "run-test262 $filter: host: $(tail -1 "$out/test262$tag.host"); Wine: $(tail -1 "$out/test262$tag.wine"); $same"
		done
	fi
	sync
fi

# 4. The GPU renderer's pictures, with lavapipe on both sides.
if wants gpu; then
	wrapper "$cli" "$out/bin/browser-wine"
	pages="userland/desktop/browser/data/start.html userland/desktop/browser/data/about.html userland/desktop/browser/data/text.html
	    plan/ws074/tests/pages/blocks.html plan/ws074/tests/pages/flex.html plan/ws074/tests/pages/grid.html plan/ws074/tests/pages/floats.html"
	mkdir -p "$out/gpu-host" "$out/gpu-wine"
	# shellcheck disable=SC2086
	VK_ICD_FILENAMES=$lvp timeout 1800 $python plan/ws074/tests/gpu-compare.py $pages --program "$hostdir/browser" --out "$out/gpu-host" > "$out/gpu-host.log" 2>&1
	# shellcheck disable=SC2086
	timeout 3600 $python plan/ws074/tests/gpu-compare.py $pages --program "$out/bin/browser-wine" --out "$out/gpu-wine" > "$out/gpu-wine.log" 2>&1
	say "gpu-compare, host (GPU against CPU on one side): $(grep -c ': agree' "$out/gpu-host.log") agree, $(grep -c ': DIFFER' "$out/gpu-host.log") differ"
	say "gpu-compare, Wine (GPU against CPU on one side): $(grep -c ': agree' "$out/gpu-wine.log") agree, $(grep -c ': DIFFER' "$out/gpu-wine.log") differ"
	for page in $pages; do
		base=$(basename "$page" .html)
		for kind in gpu cpu; do
			if [ -f "$out/gpu-host/$base-$kind.ppm" ] && [ -f "$out/gpu-wine/$base-$kind.ppm" ]; then
				line=$($python plan/ws334/tests/compare-ppm.py "$base $kind (host against Wine)" "$out/gpu-host/$base-$kind.ppm" "$out/gpu-wine/$base-$kind.ppm")
			else
				line="$base $kind: a picture is missing (host: $(ls "$out/gpu-host/$base-$kind.ppm" 2> /dev/null | wc -l), Wine: $(ls "$out/gpu-wine/$base-$kind.ppm" 2> /dev/null | wc -l))"
			fi
			say "$line"
			case $line in
			*same|*close) ;;
			*) differences=$((differences + 1)) ;;
			esac
		done
	done
	sync
fi

# 5. Interop 2025's focus areas, reftests.
if wants interop; then
	wrapper "$cli" "$out/bin/browser-wine"
	: > "$out/interop.txt"
	for area in css/css-flexbox css/css-grid css/css-backgrounds css/css-transforms css/css-text; do
		tag=$(echo "$area" | tr '/' '-')
		mkdir -p "$out/reftest-host-$tag" "$out/reftest-wine-$tag"
		VK_ICD_FILENAMES=$lvp timeout 3600 $python plan/ws074/tests/run-wpt-reftests.py --root "$area" --limit 20 \
		    --program "$hostdir/browser" --out "$out/reftest-host-$tag" > "$out/reftest-host-$tag.log" 2>&1
		timeout 7200 $python plan/ws074/tests/run-wpt-reftests.py --root "$area" --limit 20 \
		    --program "$out/bin/browser-wine" --out "$out/reftest-wine-$tag" > "$out/reftest-wine-$tag.log" 2>&1
		line=$($python plan/ws334/tests/compare-reftests.py "reftests $area" "$out/reftest-host-$tag/report.json" "$out/reftest-wine-$tag/report.json")
		say "$line"
		case $line in
		*"other status 0"*) ;;
		*) differences=$((differences + 1)) ;;
		esac
	done
	sync
fi

say "host-tests-windows: done ($differences differences to explain; out $out)"

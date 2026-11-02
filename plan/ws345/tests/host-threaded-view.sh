#!/bin/sh
# ws345: builds and runs plan/ws345/tests/host-threaded-view.c (a view of its own threads: the window's and the drawing's stand-ins, the order, merge and bound of
# inputs, the end and navigation out of a script that runs for ever) against the engine's objects of a host build (the test uses the engine's inside, not the public interface).
#
#   sh plan/ws345/tests/host-threaded-view.sh [plain|asan|tsan]     (default plain)
#
# The engine: the objects in $BROWSER_HOST_BUILD/<variant>/obj (default build/ws345-host; host-build.sh makes them),
# all but the browser program's own.
# Last line: the test's summary; the exit status is the test's.
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
set -eu
cd "$(dirname -- "$0")/../../.."
variant=${1:-plain}
base=${BROWSER_HOST_BUILD:-build/ws345-host}
out=$base/$variant
[ -f "$out/flags" ] || { echo "host-threaded-view: no $out (run host-build.sh first)" >&2; exit 2; }
flags=$(cut -d' ' -f2- "$out/flags")
objects=$(ls "$out"/obj/*.o | grep -v -e '/browser_main' -e '/browser-os-error\.o$')
"${CC:-cc}" $flags -o "$out/host-threaded-view" plan/ws345/tests/host-threaded-view.c $objects -lvulkan -lm -ldl -lpthread
# lavapipe's own threads are not the engine's: their reports are suppressed in the tsan variant.
if [ "$variant" = tsan ]; then
	TSAN_OPTIONS="${TSAN_OPTIONS:-} suppressions=$(pwd)/plan/ws345/tests/tsan-lavapipe.supp"
	export TSAN_OPTIONS
fi
timeout 600 "$out/host-threaded-view"

#!/bin/sh
# ws345: builds and runs plan/ws345/tests/host-frames.c (the iframes' threads of a view of its own threads: an iframe's timer on its own thread,
# same-origin access and postMessage, an endless iframe, removal, the bound on frames, cross-realm reactions, an iframe's XMLHttpRequest, a kept page,
# a thread that cannot start; the frames of other agents: their own threads and heaps, messages, relatives, navigations, frame items drawn, input, ends) against the engine's objects of a host build (the test gives the engine its threads itself, so host-threads.o stays out).
#
#   sh plan/ws345/tests/host-frames.sh [plain|asan|tsan]     (default plain)
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
[ -f "$out/flags" ] || { echo "host-frames: no $out (run host-build.sh first)" >&2; exit 2; }
flags=$(cut -d' ' -f2- "$out/flags")
objects=$(ls "$out"/obj/*.o | grep -v -e '/browser_main' -e '/browser-os-error\.o$' -e '/host-threads\.o$')
"${CC:-cc}" $flags -o "$out/host-frames" plan/ws345/tests/host-frames.c $objects -lvulkan -lm -ldl -lpthread
# lavapipe's own threads are not the engine's: their reports are suppressed in the tsan variant.
if [ "$variant" = tsan ]; then
	TSAN_OPTIONS="${TSAN_OPTIONS:-} suppressions=$(pwd)/plan/ws345/tests/tsan-lavapipe.supp"
	export TSAN_OPTIONS
fi
timeout 900 "$out/host-frames"

#!/bin/sh
# ws328: builds and runs plan/ws328/tests/host-css-gradient.c (the CSS gradient functions: the parsed fields, the declarations
# the grammar rejects, the computed values and their round trip) against the engine objects of a host build (host-build.sh makes them).
#
#   sh plan/ws328/tests/host-css-gradient.sh [plain|asan|tsan]     (default asan)
#   BROWSER_HOST_BUILD=build/ws328-host CC=cc sh plan/ws074/tests/host-build.sh asan     (makes the objects first)
#
# The engine: the objects in $BROWSER_HOST_BUILD/<variant>/obj (default build/ws328-host), all but the browser program's own.
# The test gives the engine its threads itself through the same table the other host tests use (host-threads.o is linked).
# Last line: the test's summary; the exit status is the test's.
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
set -eu
cd "$(dirname -- "$0")/../../.."
variant=${1:-asan}
base=${BROWSER_HOST_BUILD:-build/ws328-host}
out=$base/$variant
[ -f "$out/flags" ] || { echo "host-css-gradient: no $out (run host-build.sh first)" >&2; exit 2; }
flags=$(cut -d' ' -f2- "$out/flags")
objects=$(ls "$out"/obj/*.o | grep -v -e '/browser_main' -e '/browser-os-error\.o$')
"${CC:-cc}" $flags -o "$out/host-css-gradient" plan/ws328/tests/host-css-gradient.c $objects -lvulkan -lm -ldl -lpthread
ASAN_OPTIONS=detect_stack_use_after_return=0 timeout 600 "$out/host-css-gradient"

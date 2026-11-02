#!/bin/sh
# ws345: builds and runs plan/ws345/tests/host-terminate.c (a task of the JavaScript engine ended from another
# thread) against the engine's objects of a host build (the test uses the engine's inside, not the public interface).
#
#   sh plan/ws345/tests/host-terminate.sh [plain|asan|tsan]     (default plain)
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
[ -f "$out/flags" ] || { echo "host-terminate: no $out (run host-build.sh first)" >&2; exit 2; }
flags=$(cut -d' ' -f2- "$out/flags")
objects=$(ls "$out"/obj/*.o | grep -v -e '/browser_main' -e '/browser-os-error\.o$')
"${CC:-cc}" $flags -o "$out/host-terminate" plan/ws345/tests/host-terminate.c $objects -lvulkan -lm -ldl -lpthread
timeout 300 "$out/host-terminate"

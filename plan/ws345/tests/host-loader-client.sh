#!/bin/sh
# ws345: builds and runs plan/ws345/tests/host-loader-client.c (the loader's clients, waited requests and
# aborts, and the page's waiting fetches through the loader) with the engine's objects of a host build,
# linked with -Wl,--wrap=connect so that the test sees which thread makes each connect.
#
#   sh plan/ws345/tests/host-loader-client.sh [plain|asan|tsan]     (default plain)
#
# The engine: $BROWSER_HOST_BUILD/<variant>/obj (default build/ws345-host; host-build.sh makes it).  The unit
# tests' host-threads.o there gives the engine the POSIX threads before main.
# Last line: the test's summary; the exit status is the test's.
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
set -eu
cd "$(dirname -- "$0")/../../.."
variant=${1:-plain}
base=${BROWSER_HOST_BUILD:-build/ws345-host}
out=$base/$variant
[ -f "$out/flags" ] || { echo "host-loader-client: no $out (run host-build.sh first)" >&2; exit 2; }
flags=$(cut -d' ' -f2- "$out/flags")
engine=$(ls "$out"/obj/*.o | grep -v -e '/browser_main\.o$' -e '/browser_main-posix\.o$' -e '/host-shell\.o$' -e '/browser-os-error\.o$')
"${CC:-cc}" $flags -o "$out/host-loader-client" plan/ws345/tests/host-loader-client.c $engine \
    -Wl,--wrap=connect -lvulkan -lm -ldl -lpthread
ASAN_OPTIONS=detect_stack_use_after_return=0 timeout 300 "$out/host-loader-client"

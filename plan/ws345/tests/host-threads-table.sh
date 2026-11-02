#!/bin/sh
# ws345: builds and runs plan/ws345/tests/host-threads-table.c (the table of the program's threads,
# browser_threads_set) against the engine of a host build, through the public interface only.
#
#   sh plan/ws345/tests/host-threads-table.sh [plain|asan|tsan]     (default plain)
#
# The engine: $BROWSER_HOST_BUILD/<variant>/libbrowser.so (default build/ws345-host; host-build.sh makes it).
# Last line: the test's summary; the exit status is the test's.
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
set -eu
cd "$(dirname -- "$0")/../../.."
variant=${1:-plain}
base=${BROWSER_HOST_BUILD:-build/ws345-host}
out=$base/$variant
[ -f "$out/libbrowser.so" ] || { echo "host-threads-table: no $out/libbrowser.so (run host-build.sh first)" >&2; exit 2; }
flags=$(cut -d' ' -f2- "$out/flags")
"${CC:-cc}" $flags -o "$out/host-threads-table" plan/ws345/tests/host-threads-table.c \
    userland/tests/browser-threads/threads-posix.c -L"$out" -Wl,-rpath,"$(pwd)/$out" -l:libbrowser.so -lpthread -lm
timeout 120 "$out/host-threads-table"

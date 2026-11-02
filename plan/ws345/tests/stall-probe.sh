#!/bin/sh
# ws345: measures how long a window's events wait while the browser's view works (stall-probe.c), every
# scenario in a process of its own, against the engine of a host build.
#
#   sh plan/ws345/tests/stall-probe.sh [SCENARIO...]     (default: all of them)
#   STALL_MODE=own sh plan/ws345/tests/stall-probe.sh ...  (the view on its own threads, BROWSER_THREADING_OWN)
#
# The engine: build/ws345-host/plain/libbrowser.so (BROWSER_HOST_BUILD=build/ws345-host sh
# plan/ws074/tests/host-build.sh plain makes it).  The probe goes to build/ws345-host/plain/stall-probe.
# Prints the probe's summary line of each scenario.
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
set -eu
cd "$(dirname -- "$0")/../../.."
out=build/ws345-host/plain
[ -f "$out/libbrowser.so" ] || { echo "stall-probe: no $out/libbrowser.so (run host-build.sh first)" >&2; exit 2; }
cc=${CC:-cc}
"$cc" -std=gnu11 -O1 -g -Wall -Wextra -Werror -D_GNU_SOURCE -I. -Ibuild/ws345-host/include -o "$out/stall-probe" \
    plan/ws345/tests/stall-probe.c userland/tests/browser-threads/threads-posix.c -L"$out" -Wl,-rpath,"$(pwd)/$out" -l:libbrowser.so -lpthread -lm
scenarios=${*:-"idle loop handler sync-xhr-connect sync-xhr-read docwrite-connect async-connect resize-big decode-big"}
for scenario in $scenarios; do
	timeout 60 "$out/stall-probe" "$scenario" 6 ${STALL_MODE:-caller} 2>"$out/stall-probe-$scenario.err" || echo "stall-probe $scenario: exit $?"
done

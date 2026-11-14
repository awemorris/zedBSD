#!/bin/sh
# ws345: the acceptance test of a view of its own threads (threaded-latency.c): in every scenario a window's events
# wait at most 20 ms and none over 100 ms, the drawing thread presents each resize within 100 ms, and the page did
# what the scenario is there for.  Each scenario runs in a process of its own, against the engine of a host build.
#
#   sh plan/ws345/tests/threaded-latency.sh [plain|tsan] [SCENARIO...]     (default plain, all the scenarios)
#
# The engine: build/ws345-host/<variant>/libbrowser.so (BROWSER_HOST_BUILD=build/ws345-host sh
# plan/ws074/tests/host-build.sh <variant> makes it).  The test goes to build/ws345-host/<variant>/threaded-latency,
# each scenario's error output (the page's console, ThreadSanitizer's reports) to threaded-latency-SCENARIO.err there.
# In the tsan variant the times are not judged (its threads run many times slower): a scenario passes when its page's
# sign came and ThreadSanitizer reported nothing (lavapipe's threads suppressed as in host-frames.sh).
# Prints each scenario's line; the last line is threaded-latency: PASS or threaded-latency: FAIL (N of M failed).
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
set -u
cd "$(dirname -- "$0")/../../.."
variant=${1:-plain}
[ $# -gt 0 ] && shift
case $variant in
plain|tsan) ;;
*) echo "threaded-latency: unknown variant $variant" >&2; exit 2 ;;
esac
out=build/ws345-host/$variant
[ -f "$out/libbrowser.so" ] || { echo "threaded-latency: no $out/libbrowser.so (run host-build.sh $variant first)" >&2; exit 2; }
flags=$(cut -d' ' -f2- "$out/flags" | sed 's/ -fPIC//')
"${CC:-cc}" $flags -Ibuild/ws345-host/include -o "$out/threaded-latency" \
    plan/ws345/tests/threaded-latency.c userland/tests/browser-threads/threads-posix.c \
    -L"$out" -Wl,-rpath,"$(pwd)/$out" -l:libbrowser.so -lpthread -lm || { echo "threaded-latency: FAIL (the test did not build)"; exit 1; }
if [ "$variant" = tsan ]; then
	TSAN_OPTIONS="${TSAN_OPTIONS:-} suppressions=$(pwd)/plan/ws345/tests/tsan-lavapipe.supp"
	NOTIME=1
	export TSAN_OPTIONS NOTIME
fi
scenarios=${*:-"idle loop handler sync-xhr-connect sync-xhr-read docwrite-connect async-connect resize-big decode-big iframe-loop"}
total=0
failed=0
for scenario in $scenarios; do
	total=$((total + 1))
	err=$out/threaded-latency-$scenario.err
	line=$(timeout 120 "$out/threaded-latency" "$scenario" 2>"$err")
	code=$?
	reports=$(grep -c 'WARNING: ThreadSanitizer' "$err")
	echo "${line:-threaded-latency $scenario: FAIL no summary line} exit=$code tsan_reports=$reports"
	if [ $code -ne 0 ] || [ "$reports" != 0 ]; then
		failed=$((failed + 1))
	fi
done
if [ $failed -eq 0 ]; then
	echo "threaded-latency: PASS ($total scenarios, $variant)"
else
	echo "threaded-latency: FAIL ($failed of $total failed, $variant)"
	exit 1
fi

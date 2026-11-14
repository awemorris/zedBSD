#!/bin/sh
# ws345 R12: the headless tests of the browser in both of a view's modes, the caller's thread
# (BROWSER_THREADING_CALLER, the program's calls run the page) and its own threads
# (BROWSER_THREADING_OWN, browser --threads: the window's way), and whether they give the same results.
#
#   sh plan/ws345/tests/r12.sh [plain|tsan] [PORT_OFFSET] [TEST...]
#
# The tests (all by default): dom (run-dom-tests.py), cross-origin (run-cross-origin-tests.py), reftests
# (run-wpt-reftests.py, its default sample of 100), http and http-async (run-http-tests.py), fetch
# (run-fetch-tests.py), component (plan/tools/browser-component, plain only) and wpt-cross-origin
# (plan/ws345/tests/wpt-cross-origin.py).  The programs are $BROWSER_HOST_BUILD/<variant> (default
# build/ws345-host; host-build.sh makes them); the reftests and wpt-cross-origin need the WPT checkout
# (fetch-suites.sh wpt).  PORT_OFFSET (default 0) is added to the network tests' ports.
# Each test runs in the caller's mode, then in the own mode, one after the other.  Its results are compared:
# the lines that name a check and its outcome (the logs' timings, temporary paths and ports aside; for fetch, its
# result lines, without the test server's reports of a connection the browser closed).
# wpt-cross-origin is not compared: the frames of other origins run in agents of their own only in a view of
# its own threads (in the caller's they are refused), so its results are each mode's own.
# Output: build/tmp/ws345-r12-<variant>/: <test>-<mode>.log, <test>-<mode>.results (the compared
# lines) and summary.txt (per test: each mode's last line, and same or differ); the last line is
# "r12: N same, M differ".
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
set -u
cd "$(dirname -- "$0")/../../.."
variant=${1:-plain}
offset=${2:-0}
[ $# -gt 0 ] && shift
[ $# -gt 0 ] && shift
tests=${*:-dom cross-origin reftests http http-async fetch component wpt-cross-origin}
case $variant in
plain|tsan) ;;
*) echo "r12: unknown variant $variant" >&2; exit 2 ;;
esac
base=${BROWSER_HOST_BUILD:-build/ws345-host}
host=$base/$variant
[ -x "$host/browser" ] || { echo "r12: no $host/browser (run host-build.sh first)" >&2; exit 2; }
out=build/tmp/ws345-r12-$variant
mkdir -p "$out/bin"
summary=$out/summary.txt
: > "$summary"

# The own mode's program: the browser with --threads before the runner's arguments.
printf '#!/bin/sh\nexec "%s" --threads "$@"\n' "$(pwd)/$host/browser" > "$out/bin/browser-threads"
chmod 755 "$out/bin/browser-threads"
if [ "$variant" = tsan ]; then
	TSAN_OPTIONS="${TSAN_OPTIONS:-} suppressions=$(pwd)/plan/ws345/tests/tsan-lavapipe.supp"
	export TSAN_OPTIONS
fi

# One test in one mode: its log and the lines compared.
# run_one TEST MODE
run_one() {
	test=$1
	mode=$2
	program=$(pwd)/$host/browser
	threads=
	if [ "$mode" = own ]; then
		program=$(pwd)/$out/bin/browser-threads
		threads=threads
	fi
	log=$out/$test-$mode.log
	case $test in
	dom) timeout 1800 /usr/bin/python3 -I plan/ws074/tests/run-dom-tests.py --program "$program" > "$log" 2>&1 ;;
	cross-origin) timeout 1800 /usr/bin/python3 -I plan/ws074/tests/run-cross-origin-tests.py --program "$program" > "$log" 2>&1 ;;
	reftests) timeout 7200 /usr/bin/python3 -I plan/ws074/tests/run-wpt-reftests.py --program "$program" --out "$out/reftests-$mode" > "$log" 2>&1 ;;
	http) timeout 1800 /usr/bin/python3 plan/ws074/tests/run-http-tests.py --program "$program" \
	    --port $((18074 + offset)) --tls-port $((18443 + offset)) --tls-wrong-port $((18444 + offset)) > "$log" 2>&1 ;;
	http-async) timeout 1800 /usr/bin/python3 plan/ws074/tests/run-http-tests.py --async --program "$program" \
	    --port $((18074 + offset)) --tls-port $((18443 + offset)) --tls-wrong-port $((18444 + offset)) > "$log" 2>&1 ;;
	fetch) timeout 1800 /usr/bin/python3 -I plan/ws074/tests/run-fetch-tests.py --program "$program" > "$log" 2>&1 ;;
	component) BROWSER_HOST_BUILD=$base timeout 1800 sh plan/tools/browser-component/run.sh "$variant" $threads > "$log" 2>&1 ;;
	wpt-cross-origin) timeout 3600 /usr/bin/python3 -I plan/ws345/tests/wpt-cross-origin.py --program "$(pwd)/$host/browser" \
	    --out "$out/wpt-cross-origin-$mode" --port $((18310 + offset)) --jobs 3 ${threads:+--threads} > "$log" 2>&1 ;;
	*) echo "r12: unknown test $test" >&2; return 2 ;;
	esac
	code=$?
	echo "$test $mode: exit $code: $(tail -1 "$log")" >> "$summary"

	# The compared lines: the outcome of each check, without timings, ports and temporary paths.
	case $test in
	reftests) grep -E '^\[[0-9]+/[0-9]+\] ' "$log" ;;
	fetch) grep -E '^(ok |FAIL|fetch-tests)' "$log" ;;
	component) grep -v -e '^built ' "$log" | sed 's/ (views of their own threads.*)$//; s/^component: [0-9]* checks, /component: /' ;;
	wpt-cross-origin) cat "$out/wpt-cross-origin-$mode/results.txt" 2>/dev/null ;;
	*) cat "$log" ;;
	esac | sed -E 's/127\.0\.0\.1:[0-9]+/127.0.0.1:PORT/g; s/[0-9]+(\.[0-9]+)? ?ms\b/N ms/g; s#/tmp/[^ ]*#TMP#g' > "$out/$test-$mode.results"
	return 0
}

# Each test in both modes, then the comparison.
same=0
differ=0
for test in $tests; do
	run_one "$test" caller
	run_one "$test" own
	if [ "$test" = wpt-cross-origin ]; then
		echo "$test: not compared (other origins' agents only in the own mode)" >> "$summary"
	elif cmp -s "$out/$test-caller.results" "$out/$test-own.results"; then
		echo "$test: same" >> "$summary"
		same=$((same + 1))
	else
		echo "$test: differ (diff $out/$test-caller.results $out/$test-own.results)" >> "$summary"
		differ=$((differ + 1))
	fi
done
sync
echo "r12: $same same, $differ differ" >> "$summary"
cat "$summary"
[ "$differ" -eq 0 ]

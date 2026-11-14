#!/bin/sh
# The first stage of the browser's host tests on one tree, for counting before and after a change:
# the WS074 host build and its host-* tests, the JavaScript tests, the network tests (loader, HTTP with
# and without --async, fetch) and the browser component, for one variant.
#   sh plan/tools/browser-win32/stage1.sh TREE plain|asan [PORT_OFFSET]
# TREE is the root of a source tree (this one, or a git archive of another commit); everything is built
# and run inside it.  PORT_OFFSET (default 0) is added to the network tests' ports, so that two trees can
# run at the same time.  The summary (one line per check, "name: result") goes to
# TREE/build/tmp/browser-win32-stage1-VARIANT/summary.txt and to the standard output; host-exits.txt there has
# each host-* test's exit status.
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
set -u
tree=$1
variant=$2
offset=${3:-0}
case $variant in
plain|asan) ;;
*) echo "stage1: unknown variant $variant" >&2; exit 2 ;;
esac
cd "$tree" || exit 2
out=build/tmp/browser-win32-stage1-$variant
mkdir -p "$out"
summary=$out/summary.txt
: > "$summary"
ASAN_OPTIONS=detect_stack_use_after_return=0
export ASAN_OPTIONS
host=build/ws074-host/$variant

# The host build: its exit status, its warnings and how many programs it built.
timeout 3600 sh plan/ws074/tests/host-build.sh "$variant" > "$out/host-build.log" 2>&1
echo "host-build: exit $? warnings $(grep -c 'warning:' "$out/host-build.log") built $(grep -c '^built ' "$out/host-build.log")" >> "$summary"

# Each host-* test without arguments (the ones that need arguments exit 2), 600 s each.
: > "$out/host-exits.txt"
for program in "$host"/host-*; do
	[ -x "$program" ] || continue
	timeout 600 "$program" > "$out/$(basename "$program").log" 2>&1
	echo "$(basename "$program") $?" >> "$out/host-exits.txt"
done
echo "host-tests: $(awk '$2 == 0' "$out/host-exits.txt" | wc -l) exit 0, $(awk '$2 != 0' "$out/host-exits.txt" | wc -l) other" >> "$summary"

# A runner's last line into the summary.
run() {
	name=$1
	shift
	timeout 3600 "$@" > "$out/$name.log" 2>&1
	code=$?
	echo "$name: exit $code: $(tail -1 "$out/$name.log")" >> "$summary"
}

# The JavaScript tests and the network tests on the variant's programs.
run js /usr/bin/python3 plan/ws074/tests/run-js-tests.py --program "$host/browser"
run loader /usr/bin/python3 plan/ws074/tests/run-loader-tests.py --program "$host/host-loader" \
    --port $((18174 + offset)) --tls-port $((18543 + offset)) --tls-wrong-port $((18544 + offset))
run http /usr/bin/python3 plan/ws074/tests/run-http-tests.py --program "$host/browser" \
    --port $((18074 + offset)) --tls-port $((18443 + offset)) --tls-wrong-port $((18444 + offset))
run http-async /usr/bin/python3 plan/ws074/tests/run-http-tests.py --async --program "$host/browser" \
    --port $((18074 + offset)) --tls-port $((18443 + offset)) --tls-wrong-port $((18444 + offset))
run fetch /usr/bin/python3 plan/ws074/tests/run-fetch-tests.py --program "$host/browser"
run component sh plan/tools/browser-component/run.sh "$variant"
sync
cat "$summary"

#!/bin/sh
# Runs the host tests that look at the browser's pages with a browser program under BROWSER_LAYOUT_CACHE=off and
# under verify (the cache's work compared with the whole layout and style, differences told on standard error), and
# prints each test's last line under both modes with the number of differences verify found.
#   sh plan/ws346/tests/run-verify.sh [PROGRAM]      (default build/ws074-host/plain/browser)
# A test that passes as many under verify as under off, and a difference count of 0, is what a cache that is right shows.
# The tests that start the program themselves take it as --program; the scripted reftests (their own client
# program) are run apart.  Run from the repository's root.
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
set -u
cd "$(dirname -- "$0")/../../.."
program=$(cd "$(dirname -- "${1:-build/ws074-host/plain/browser}")" && pwd)/$(basename -- "${1:-build/ws074-host/plain/browser}")
work=build/tmp/ws346/verify
mkdir -p "$work"
tests="plan/ws074/tests/run-dom-tests.py plan/ws074/tests/run-js-tests.py plan/ws074/tests/run-fetch-tests.py plan/ws074/tests/run-wpt-reftests.py plan/ws074/tests/run-acid-tests.py"
[ -n "${VERIFY_TESTS:-}" ] && tests=$VERIFY_TESTS

# A program that runs the browser and keeps what it says on standard error for counting.
wrapper=$work/browser
cat > "$wrapper" <<'EOF'
#!/bin/bash
exec "$REAL_BROWSER" "$@" 2> >(tee -a "$VERIFY_LOG" >&2)
EOF
chmod +x "$wrapper"
wrapper=$(pwd)/$wrapper

total=0
for mode in off verify; do
	log=$(pwd)/$work/$mode.log
	: > "$log"
	echo "== BROWSER_LAYOUT_CACHE=$mode"
	for test in $tests; do
		name=$(basename "$test" .py)
		out=$(REAL_BROWSER=$program VERIFY_LOG=$log BROWSER_LAYOUT_CACHE=$mode timeout 3000 python3 "$test" --program "$wrapper" 2>&1 | tail -3 | tr '\n' ' ')
		echo "  $name: $out"
	done
	sleep 1
	count=$(grep -c 'layout-cache: .*mismatch' "$log")
	echo "  differences: $count"
	[ "$mode" = verify ] && total=$count
done
[ "$total" -eq 0 ]

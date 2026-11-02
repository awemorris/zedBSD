#!/bin/sh
# Times plan/ws074/tests/bench/bug411.html (property lookup in large objects, getComputedStyle after a change,
# compiling a large scope) with one or more browser programs, three runs each, and prints the median of each line.
#   sh plan/ws074/tests/bench/bug411.sh PROGRAM...     (e.g. build/ws074-host/plain/browser)
# Run from the repository's root.
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
set -u
cd "$(dirname -- "$0")/../../../.."
fonts="--font=userland/desktop/fonts/Mahora-Regular.ttf --mono-font=userland/desktop/fonts/JetBrainsMono-Regular.ttf --fallback-font=userland/desktop/fonts/DroidSansFallbackFull.ttf"
for program in "$@"; do
	runs=""
	for run in 1 2 3; do
		runs="$runs
$(XDG_DATA_HOME=build/ws074-host-data timeout 600 "$program" --run $fonts plan/ws074/tests/bench/bug411.html 2>&1 | grep '^bench ')"
	done
	echo "$program"
	printf '%s\n' "$runs" | awk 'NF == 4 { times[$2] = times[$2] " " $3; check[$2] = $4 }
	    END { for (name in times) { n = split(substr(times[name], 2), t, " "); for (i = 1; i <= n; i++) for (j = i + 1; j <= n; j++) if (t[j] < t[i]) { x = t[i]; t[i] = t[j]; t[j] = x }
	        printf "  %-24s %6d ms (median of %d)  check %s\n", name, t[int((n + 1) / 2)], n, check[name] } }' | sort
done

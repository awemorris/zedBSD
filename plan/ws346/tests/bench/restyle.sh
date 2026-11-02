#!/bin/sh
# Times plan/ws346/tests/bench/restyle.html (what a change of the document costs the style and the layout when a
# script reads geometry or style after it) with a browser program under each mode of the cache, three runs each,
# and prints the median of each line.
#   sh plan/ws346/tests/bench/restyle.sh PROGRAM [MODE...]     (e.g. build/ws074-host/plain/browser off on)
# MODE is a value of BROWSER_LAYOUT_CACHE; without one, off and on are timed.  Run from the repository's root.
# A line's check (the page's own sum) must be the same in every mode; a different one is printed as MISMATCH.
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
set -u
cd "$(dirname -- "$0")/../../../.."
program=${1:?usage: restyle.sh PROGRAM [MODE...]}
shift
[ $# -eq 0 ] && set -- off on
fonts="--font=userland/desktop/fonts/Mahora-Regular.ttf --mono-font=userland/desktop/fonts/JetBrainsMono-Regular.ttf --fallback-font=userland/desktop/fonts/DroidSansFallbackFull.ttf"
for mode in "$@"; do
	runs=""
	for run in 1 2 3; do
		runs="$runs
$(BROWSER_LAYOUT_CACHE=$mode XDG_DATA_HOME=build/ws074-host-data timeout 1200 "$program" --run $fonts plan/ws346/tests/bench/restyle.html 2>&1 | grep '^bench ')"
	done
	echo "$program (BROWSER_LAYOUT_CACHE=$mode)"
	printf '%s\n' "$runs" | awk 'NF == 4 { times[$2] = times[$2] " " $3; if ($2 in check && check[$2] != $4) bad[$2] = 1; check[$2] = $4 }
	    END { for (name in times) { n = split(substr(times[name], 2), t, " "); for (i = 1; i <= n; i++) for (j = i + 1; j <= n; j++) if (t[j] < t[i]) { x = t[i]; t[i] = t[j]; t[j] = x }
	        printf "  %-24s %7d ms (median of %d)  check %s%s\n", name, t[int((n + 1) / 2)], n, check[name], (name in bad) ? "  MISMATCH between runs" : "" } }' | sort
done

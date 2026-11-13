#!/bin/sh
# Guest side of qemu-ui-deadlines.sh: sums the compositor's "KWL PERF timers" lines after line FROM of LOG and prints
# one line "timers fired=T NAME=COUNT ..." (the timers that fired in those windows, by name).
#   sh /root/perf-timers.sh FROM LOG
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
awk -v from="$1" '
NR > from && /^KWL PERF timers fired=[0-9]+/ {
	split($4, f, "="); total += f[2]
	for (i = 5; i <= NF; i++) {
		split($i, pair, "=")
		if (!(pair[1] in counts))
			names[++kinds] = pair[1]
		counts[pair[1]] += pair[2]
	}
}
END {
	line = "timers fired=" (total + 0)
	for (i = 1; i <= kinds; i++)
		if (counts[names[i]] > 0)
			line = line " " names[i] "=" counts[names[i]]
	print line
}
' "$2"

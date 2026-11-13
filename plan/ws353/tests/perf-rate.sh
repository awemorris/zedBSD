#!/bin/sh
# Guest side of qemu-main-wake.sh: sums the compositor's KWL PERF windows after line FROM of LOG and prints one line
# "ms=M passes=P timeouts=T lines=N" (the event loop's passes, and those whose poll ended on its timeout).
#   sh /root/perf-rate.sh FROM LOG
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
awk -v from="$1" '
NR > from && /^KWL PERF [0-9]+ms: passes=[0-9]+ timeouts=[0-9]+ / {
	ms += substr($3, 1, length($3) - 3)
	split($4, p, "="); passes += p[2]
	split($5, t, "="); timeouts += t[2]
	lines++
}
END { printf "ms=%d passes=%d timeouts=%d lines=%d\n", ms, passes, timeouts, lines }
' "$2"

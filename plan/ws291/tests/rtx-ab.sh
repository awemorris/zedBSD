#!/bin/sh
# ws291 (NVIDIA): kernels measured in turn on the RTX 2070, each in a window of its own (rtx-bench.sh with rtx-ab.list),
# the order given then reversed (A B C, C B A), the machine back to Debian with RESTORE at the end of each.
#   sh plan/ws291/tests/rtx-ab.sh OUT RESTORE NAME=VMUNIX...   (from the top of the tree, in the RDNA machine's lock)
# OUT/NAME-ROUND/ holds each window's output; the fps lines are printed as "NAME ROUND LABEL fps".
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
set -u
out=${1:?usage: rtx-ab.sh OUT RESTORE NAME=VMUNIX...}
restore=${2:?the kernel to put back}
shift 2
order="$*"
reverse=""
for arm in $order; do reverse="$arm $reverse"; done
mkdir -p "$out"
round=0
for list in "$order" "$reverse"; do
	round=$((round + 1))
	for arm in $list; do
		name=${arm%%=*}; kernel=${arm#*=}
		timeout 1500 sh plan/ws291/tests/rtx-bench.sh "$out/$name-$round" "$kernel" "$restore" plan/ws291/tests/rtx-ab.list > "$out/$name-$round.log" 2>&1
		grep -E "^(big|subs|scene)-[0-9]+ " "$out/$name-$round.log" | awk -v n="$name" -v r="$round" '{ print n, r, $0 }'
		echo "$name $round done $(date +%H:%M)"
	done
done

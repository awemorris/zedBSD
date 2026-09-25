#!/bin/sh
# ws046: runs the make cases that make-diff.py --export wrote, with this
# system's make (/usr/bin/make), each in an empty directory of its own, and
# writes NNNN.out (standard output) and NNNN.status beside each NNNN.sh;
# make-diff.py --compare reads them back on the host.
#   sh make-guest.sh CASES_DIR
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
cases=$1
count=0
for code in "$cases"/*.sh; do
	case=${code%.sh}
	count=$((count + 1))
	work=/tmp/ws046-work.$$.$count
	mkdir -p "$work"
	cd "$work" || exit 1
	env -i PATH=/bin:/usr/bin HOME="$work" LC_ALL=C MAKE=/usr/bin/make MAKEFLAGS= \
	    timeout 30 /bin/sh "$code" > "$case.out" 2>/dev/null </dev/null
	echo $? > "$case.status"
	cd /
	rm -rf "$work"
done
echo "RAN $count"

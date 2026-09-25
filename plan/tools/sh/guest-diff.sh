#!/bin/sh
# ws042: runs the cases sh-diff.py --export wrote, with this system's /bin/sh,
# and compares each with the reference shell's output recorded on the host.
#   [DUMP=1] guest-diff.sh CASES_DIR [REPO_ROOT]
# Each case is GG/NNNN.sh (the code) and GG/NNNN.exp (the status, the name,
# then the output), a hundred to a directory GG.  Prints FAIL and the name for each difference, then PASS.
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
cases=$1
root=${2:-/tmp/oils}
work_base=/tmp/ws042-work.$$
total=0
passed=0
for code in "$cases"/*/*.sh; do
	case=${code%.sh}
	total=$((total + 1))
	# A directory of its own for each case, so that nothing is left from
	# the one before; its files go after it (rm -f and rmdir, which do not
	# need rm -r).
	work=$work_base.$total
	mkdir -p "$work"
	cd "$work" || exit 1
	# The code is case.sh in the case's directory, as on the host.
	cp "$code" "$work/case.sh"
	env -i PATH=/bin:/usr/bin HOME="$work" TMP="$work" SH=/bin/sh \
	    LC_ALL=C REPO_ROOT="$root" \
	    timeout 10 /bin/sh "$work/case.sh" > /tmp/ws042-out 2>/dev/null </dev/null
	status=$?
	cd /
	rm -rf "$work"
	{ read expected; read name; cat > /tmp/ws042-want; } < "$case.exp"
	if cmp -s /tmp/ws042-out /tmp/ws042-want && [ "$status" = "$expected" ]; then
		passed=$((passed + 1))
	else
		echo "FAIL $name (status $status, expected $expected)"
		if [ -n "${DUMP-}" ]; then
			echo "--- got"; cat /tmp/ws042-out
			echo "--- want"; cat /tmp/ws042-want
		fi
	fi
done
echo "PASS $passed/$total"

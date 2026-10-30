#!/bin/sh
# WS049: compares the namespaces, the device evaluations (before and after
# initialization) and the evaluations of every method without arguments of
# every DSDT in QEMU's ACPI test data.
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
#
#   plan/tools/aml-host/compare-corpus.sh [QEMU_SOURCE]
#
# QEMU_SOURCE defaults to ~/qemu-pc98 (a QEMU source tree; its
# tests/data/acpi holds the expected tables of each machine and option).
# Each DSDT is loaded with the SSDTs of the same variant suffix.  Prints one
# line per table set and a summary; exits 1 when any set differs.
set -eu
repo=$(cd "$(dirname "$0")/../../.." && pwd)
source=${1:-$HOME/qemu-pc98}
data="$source/tests/data/acpi"
pass=0
fail=0
for dsdt in $(find "$data" -name 'DSDT*' -type f | sort); do
	dir=$(dirname "$dsdt")
	base=$(basename "$dsdt")
	suffix=${base#DSDT}
	set -- "$dsdt"
	for ssdt in "$dir"/SSDT*"$suffix"; do
		[ -f "$ssdt" ] || continue
		[ "$(basename "$ssdt")" = "SSDT$suffix" ] || [ -z "$suffix" ] || continue
		set -- "$@" "$ssdt"
	done
	name=$(echo "${dsdt#$data/}" | tr '/' '_')
	if "$repo/plan/tools/aml-host/compare-namespace.sh" "$name" "$@" > /dev/null &&
	    python3 "$repo/plan/tools/aml-host/compare-devices.py" "$name" "$@" > /dev/null &&
	    python3 "$repo/plan/tools/aml-host/compare-devices.py" --methods "$name-methods" "$@" > /dev/null &&
	    python3 "$repo/plan/tools/aml-host/compare-devices.py" --init "$name-init" "$@" > /dev/null; then
		pass=$((pass + 1))
	else
		fail=$((fail + 1))
		echo "DIFFERENT: $name"
	fi
done
echo "corpus: $pass same, $fail different"
[ "$fail" -eq 0 ]

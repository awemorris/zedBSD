#!/bin/sh
# WS049: loads tables with aml-host and with acpiexec and compares the namespaces.
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
#
#   plan/tools/aml-host/compare-namespace.sh NAME TABLE...
#
# Writes build/aml-host/compare/NAME-{ours,oracle}.txt and the diff; exits 0
# when the two namespaces are the same.  The DSDT must come first.
set -eu
repo=$(cd "$(dirname "$0")/../../.." && pwd)
name=$1
shift
out="$repo/build/aml-host/compare"
mkdir -p "$out"
host="$repo/build/aml-host/aml-host"
"$host" --quiet --dump "$@" > "$out/$name-ours.txt" || true
acpiexec -l -di -b "namespace" "$@" 2>/dev/null |
	python3 "$repo/plan/tools/aml-host/acpiexec-namespace.py" > "$out/$name-oracle.txt"
if diff "$out/$name-oracle.txt" "$out/$name-ours.txt" > "$out/$name.diff"; then
	echo "$name: same ($(wc -l < "$out/$name-ours.txt") nodes)"
	exit 0
fi
echo "$name: DIFFERENT ($(grep -c '^<' "$out/$name.diff" || true) only in acpiexec," \
	"$(grep -c '^>' "$out/$name.diff" || true) only in aml-host), see $out/$name.diff"
exit 1

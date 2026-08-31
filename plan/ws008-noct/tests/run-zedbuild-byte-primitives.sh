#!/bin/sh
# zedBSD-owned little-endian helpers must not depend on an optional Noct API.
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
set -eu

root=$(CDPATH= cd -- "$(dirname -- "$0")/../../.." && pwd)
noct=${NOCT:-$root/build/NoctLang/build-static/noct}
script=plan/ws008-noct/tests/zedbuild-byte-primitives.noct

cd "$root"
for mode in -j0 -j; do
	"$noct" "$mode" --path=tools/build "$script"
	for invalid in negative-offset short-buffer wrong-packed-type \
	    non-integer-value; do
		if "$noct" "$mode" --path=tools/build "$script" "$invalid" \
		    >/dev/null 2>&1; then
			echo "zedbuild byte primitive accepted $invalid in $mode" >&2
			exit 1
		fi
	done
done
if rg -n '\\bBinary\\.' tools/build/zedbuild.noct; then
	echo 'zedbuild still depends on the optional Binary API' >&2
	exit 1
fi
echo 'zedbuild byte primitive gate: PASS'

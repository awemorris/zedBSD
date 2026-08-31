#!/bin/sh
# Exercise the exact userland source-header audit with negative fixtures.
set -eu

repo=$(CDPATH= cd -- "$(dirname -- "$0")/../../.." && pwd)
audit=$repo/plan/ws001-posix/tests/userland-file-header-audit.py
temporary=$(mktemp -d "${TMPDIR:-/tmp}/zedbsd-header-audit.XXXXXX")
trap 'rm -rf -- "$temporary"' EXIT HUP INT TERM

canonical='/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */'
modeline='/* -*- coding: utf-8; tab-width: 8; indent-tabs-mode: t; -*- */'

make_good()
{
	path=$1
	mkdir -p "$(dirname "$path")"
	printf '%s\n\n%s\n\n/*\n * Implements the fixture command.\n */\n\nint fixture;\n' \
		"$modeline" "$canonical" >"$path"
}

expect_rejected()
{
	name=$1
	if python3 "$audit" --root "$temporary/$name" --expected-count 1 \
	    >/dev/null 2>&1; then
		echo "USERLAND-HEADER-T002 accepted invalid fixture: $name" >&2
		exit 1
	fi
}

make_good "$temporary/good/sample.c"
python3 "$audit" --root "$temporary/good" --expected-count 1 >/dev/null

make_good "$temporary/bom/sample.c"
printf '\357\273\277' >"$temporary/bom/prefix"
cat "$temporary/bom/sample.c" >>"$temporary/bom/prefix"
mv "$temporary/bom/prefix" "$temporary/bom/sample.c"
expect_rejected bom

make_good "$temporary/modeline/sample.c"
sed -i '1d' "$temporary/modeline/sample.c"
expect_rejected modeline

mkdir -p "$temporary/combined"
printf '%s\n' \
	'/* Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib */' \
	'int fixture;' >"$temporary/combined/sample.c"
expect_rejected combined

make_good "$temporary/owner/sample.c"
sed -i 's/Awe Morris/Another Owner/' "$temporary/owner/sample.c"
expect_rejected owner

make_good "$temporary/year/sample.c"
sed -i 's/2026/2025/' "$temporary/year/sample.c"
expect_rejected year

make_good "$temporary/license/sample.c"
sed -i 's/Zlib/MIT/' "$temporary/license/sample.c"
expect_rejected license

mkdir -p "$temporary/missing"
printf '%s\n\nint fixture;\n' "$canonical" >"$temporary/missing/sample.c"
expect_rejected missing

echo "USERLAND-HEADER-T002 audit fixtures: PASS"

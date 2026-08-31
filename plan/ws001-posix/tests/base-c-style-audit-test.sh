#!/bin/sh
# Exercise the objective base C style audit with positive and negative files.
set -eu

repo=$(CDPATH= cd -- "$(dirname -- "$0")/../../.." && pwd)
audit=$repo/plan/ws001-posix/tests/base-c-style-audit.sh
temporary=$(mktemp -d "${TMPDIR:-/tmp}/zedbsd-base-style.XXXXXX")
trap 'rm -rf -- "$temporary"' EXIT HUP INT TERM

cd "$repo"
sh "$audit" userland/base/common/command.c userland/base/common/command.h

mkdir -p "$temporary/userland/base/sample"
printf '%s\n' \
	'/*' \
	' * zedBSD' \
	' * Copyright (C) 2026 Awe Morris' \
	' *' \
	' * SPDX-License-Identifier: Zlib' \
	' */' \
	'' \
	'/*' \
	' * Declares the sample interface.' \
	' */' \
	'' \
	'/* -*- coding: utf-8; tab-width: 8; indent-tabs-mode: t; -*- */' \
	'int sample(void);' >"$temporary/userland/base/sample/good.h"
(
	cd "$temporary"
	sh "$audit" userland/base/sample/good.h
)

printf '%s\n' \
	'/*' \
	' * zedBSD' \
	' * Copyright (C) 2026 Awe Morris' \
	' *' \
	' * SPDX-License-Identifier: Zlib' \
	' */' \
	'' \
	'/*' \
	' * Declares the missing-modeline fixture.' \
	' */' \
	'' \
	'int missing_modeline(void);' \
	>"$temporary/userland/base/sample/bad-modeline.h"
if (
	cd "$temporary"
	sh "$audit" userland/base/sample/bad-modeline.h >/dev/null 2>&1
); then
	echo "BASE-STYLE-T002 missing modeline was accepted" >&2
	exit 1
fi

printf '%s\n' \
	'/*' \
	' * zedBSD' \
	' * Copyright (C) 2026 Awe Morris' \
	' *' \
	' * SPDX-License-Identifier: Zlib' \
	' */' \
	'' \
	'/*' \
	' * Exercises the same-line case fixture.' \
	' */' \
	'' \
	'/* -*- coding: utf-8; tab-width: 8; indent-tabs-mode: t; -*- */' \
	'case 1: return 0;' >"$temporary/userland/base/sample/bad-case.c"
if (
	cd "$temporary"
	sh "$audit" userland/base/sample/bad-case.c >/dev/null 2>&1
); then
	echo "BASE-STYLE-T002 same-line case statement was accepted" >&2
	exit 1
fi

echo "BASE-STYLE-T002 objective checker fixtures: PASS"

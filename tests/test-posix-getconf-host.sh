#!/usr/bin/env bash
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
set -euo pipefail

# POSIX-UTILITY-TEST: getconf positive negative

repo="$(cd "$(dirname "$0")/.." && pwd)"
work="$(mktemp -d "${TMPDIR:-/tmp}/zedbsd-getconf.XXXXXX")"
trap 'rm -rf "$work"' EXIT

cc -std=c11 -D_DEFAULT_SOURCE -DGETCONF_NO_TEXTDOMAIN -O2 -Wall \
	-Wextra -Werror -I"$repo" "$repo/userland/base/getconf/main.c" \
	-o "$work/getconf"

case "$($work/getconf ARG_MAX)" in
	''|*[!0-9]*)
		echo 'getconf ARG_MAX did not return an integer' >&2
		exit 1
		;;
esac
test "$($work/getconf PATH)" = /bin:/usr/bin
test "$($work/getconf NAME_MAX "$work")" -gt 0
test "$($work/getconf BC_BASE_MAX)" = 99
test "$($work/getconf -v POSIX_V8_LP64_OFF64 \
	POSIX_V8_LP64_OFF64_CFLAGS)" = -m64
"$work/getconf" -a "$work" >"$work/all"
grep -Eq '^ARG_MAX +[0-9]+$' "$work/all"
grep -Eq '^NAME_MAX +[0-9]+$' "$work/all"
grep -Eq '^PATH +/bin:/usr/bin$' "$work/all"

if "$work/getconf" DOES_NOT_EXIST >/dev/null 2>&1; then
	echo 'getconf accepted an unknown variable' >&2
	exit 1
fi
if "$work/getconf" NAME_MAX >/dev/null 2>&1; then
	echo 'getconf accepted a path variable without a pathname' >&2
	exit 1
fi
if "$work/getconf" -v POSIX_V8_ILP32_OFF32 ARG_MAX >/dev/null 2>&1; then
	echo 'getconf accepted an unsupported programming environment' >&2
	exit 1
fi

echo 'zedBSD POSIX getconf host tests: PASS'

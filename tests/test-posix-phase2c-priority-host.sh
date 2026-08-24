#!/usr/bin/env bash
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
set -euo pipefail

# POSIX-UTILITY-TEST: nice positive negative
# POSIX-UTILITY-TEST: renice positive negative

repo="$(cd "$(dirname "$0")/.." && pwd)"
work="$(mktemp -d "${TMPDIR:-/tmp}/zedbsd-posix-priority.XXXXXX")"
child=
trap 'test -z "$child" || kill "$child" 2>/dev/null || true; rm -rf "$work"' EXIT

cc -std=c11 -D_DEFAULT_SOURCE -O2 -Wall -Wextra -Werror -I"$repo" \
	"$repo/userland/base/nice/main.c" \
	"$repo/userland/base/common/command.c" -o "$work/nice"
cc -std=c11 -D_DEFAULT_SOURCE -O2 -Wall -Wextra -Werror \
	"$repo/userland/base/renice/main.c" -o "$work/renice"
cc -std=c11 -D_DEFAULT_SOURCE -O2 -Wall -Wextra -Werror \
	"$repo/tests/priority-host-helper.c" -o "$work/priority"

baseline="$($work/priority)"
adjusted="$($work/nice -n 3 "$work/priority")"
test "$adjusted" -gt "$baseline"
if "$work/nice" -n invalid "$work/priority" >/dev/null 2>&1; then
	echo 'nice accepted an invalid increment' >&2
	exit 1
fi
set +e
"$work/nice" does-not-exist-zedbsd >/dev/null 2>&1
missing_status=$?
set -e
test "$missing_status" -eq 127

sleep 30 &
child=$!
before="$($work/priority "$child")"
"$work/renice" -p -n 2 "$child" >"$work/renice-output"
after="$($work/priority "$child")"
test "$after" -gt "$before"
grep -q "old priority $before, new priority $after" "$work/renice-output"
if "$work/renice" -n invalid "$child" >/dev/null 2>&1; then
	echo 'renice accepted an invalid increment' >&2
	exit 1
fi
if "$work/renice" -n 1 99999999 >/dev/null 2>&1; then
	echo 'renice accepted a nonexistent process' >&2
	exit 1
fi

echo 'zedBSD POSIX priority utility host tests: PASS'

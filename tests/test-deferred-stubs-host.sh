#!/usr/bin/env bash
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
set -euo pipefail

# POSIX-UTILITY-TEST: lp positive negative

repo="$(cd "$(dirname "$0")/.." && pwd)"
work="$(mktemp -d "${TMPDIR:-/tmp}/zedbsd-deferred-stubs.XXXXXX")"
trap 'rm -rf "$work"' EXIT

cc -std=c11 -O2 -Wall -Wextra -Werror \
	"$repo/userland/base/deferred-stub/main.c" -o "$work/deferred-stub"

for command in at batch crontab logger mailx talk lp; do
	ln -s deferred-stub "$work/$command"
done

before="$(find "$work" -mindepth 1 -printf '%P %y\n' | sort)"
for command in at batch crontab logger mailx talk lp; do
	case "$command" in
	at) reason='job scheduling service is unavailable' ;;
	batch) reason='batch scheduling service is unavailable' ;;
	crontab) reason='periodic scheduling service is unavailable' ;;
	logger) reason='system logging facility is unavailable' ;;
	mailx) reason='mail provider is not installed' ;;
	talk) reason='talk rendezvous service is unavailable' ;;
	lp) reason='no print destination is configured' ;;
	esac
	set +e
	printf 'input that must not be consumed\n' | timeout 2 \
		"$work/$command" --malformed-option operand \
		>"$work/stdout" 2>"$work/stderr"
	status=$?
	set -e
	if test "$status" -eq 0 || test "$status" -eq 124; then
		echo "$command did not fail promptly (status $status)" >&2
		exit 1
	fi
	test ! -s "$work/stdout"
	test "$(cat "$work/stderr")" = "$command: $reason"
	rm -f "$work/stdout" "$work/stderr"
done
after="$(find "$work" -mindepth 1 -printf '%P %y\n' | sort)"
test "$before" = "$after"

echo 'zedBSD deferred command host tests: PASS'

#!/bin/sh
# Check mechanically decidable parts of plan/coding-style.md.
set -eu

if [ "$#" -eq 0 ]; then
	set -- userland/base/common/command.c userland/base/common/command.h
fi

failed=0

for source do
	case "$source" in
	userland/base/*.c | userland/base/*.h | userland/base/*/*.c | userland/base/*/*.h)
		;;
	*)
		echo "base-c-style: outside userland/base: $source" >&2
		failed=1
		continue
		;;
	esac

	if ! sed -n '1,20p' "$source" |
	    grep -Fq -- '-*- coding: utf-8; tab-width: 8; indent-tabs-mode: t; -*-'; then
		echo "base-c-style: missing modeline: $source" >&2
		failed=1
	fi

	if grep -n '[[:blank:]]$' "$source" >/dev/null; then
		echo "base-c-style: trailing whitespace: $source" >&2
		failed=1
	fi

	if grep -En '^[[:space:]]*(case .+|default):[[:space:]]+[^/[:space:]]' \
	    "$source" >/dev/null; then
		echo "base-c-style: statement on case label: $source" >&2
		failed=1
	fi
done

if [ "$failed" -ne 0 ]; then
	exit 1
fi

echo "BASE-STYLE-T001 objective source audit: PASS"

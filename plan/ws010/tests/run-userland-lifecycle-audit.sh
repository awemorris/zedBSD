#!/bin/sh
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib

set -eu

script_dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
repo=$(CDPATH= cd -- "$script_dir/../../.." && pwd)
temporary=$(mktemp -d)
trap 'rm -rf -- "$temporary"' EXIT HUP INT TERM

list=$temporary/makefiles
git -C "$repo" ls-files --cached --others --exclude-standard \
	'userland/**/Makefile' | while IFS= read -r file; do
	test -f "$repo/$file" || continue
	printf '%s\n' "$file"
done >"$list"

test -s "$list"

while IFS= read -r file; do
	directory=${file%/Makefile}
	log=$temporary/$(printf '%s' "$directory" | tr '/' '_').log
	for preparation_target in download patch; do
		if ! make -C "$repo/$directory" --no-print-directory -n \
			ZEDBSD_STANDALONE_CONFIG="$temporary/missing-config.mk" \
			"$preparation_target" >>"$log" 2>&1; then
			echo "userland lifecycle audit: config-free $preparation_target target failed: $file" >&2
			cat "$log" >&2
			exit 1
		fi
	done
	database=$log.database
	status=0
	make -C "$repo/$directory" --no-print-directory -qp \
		ZEDBSD_STANDALONE_CONFIG="$temporary/missing-config.mk" \
		download >"$database" 2>>"$log" || status=$?
	if test "$status" -gt 1; then
		echo "userland lifecycle audit: target database failed: $file" >&2
		cat "$log" >&2
		exit 1
	fi
	for target in download patch build install; do
		if ! grep -Eq "^$target:{1,2}([[:space:]]|$)" "$database"; then
			echo "userland lifecycle audit: missing $target target: $file" >&2
			exit 1
		fi
	done
done <"$list"

printf 'userland lifecycle audit: %s Makefiles expose download/patch/build/install\n' \
	"$(wc -l <"$list" | tr -d '[:space:]')"

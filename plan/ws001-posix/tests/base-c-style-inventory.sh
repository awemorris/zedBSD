#!/bin/sh
# List style-adoption state for every base C and header file.
set -eu

repo=$(CDPATH= cd -- "$(dirname -- "$0")/../../.." && pwd)

find "$repo/userland/base" -type f \( -name '*.c' -o -name '*.h' \) \
	-not -path '*/build/*' -print |
	sort |
	while IFS= read -r source; do
		relative=${source#"$repo/"}
		if sed -n '1,20p' "$source" |
		    grep -Fq -- '-*- coding: utf-8; tab-width: 8; indent-tabs-mode: t; -*-'; then
			printf 'compliant\t%s\n' "$relative"
		else
			printf 'historical\t%s\n' "$relative"
		fi
	done

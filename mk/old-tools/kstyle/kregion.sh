#!/bin/sh
# kregion.sh SRC REGION...
#
# Prints the named consolidated regions of SRC as one compilable translation
# unit.  A consolidated source marks every absorbed file with
#   /* Begin consolidated <name>. */ ... /* End consolidated <name>. */
# and keeps that file's own includes inside the region, so an extracted
# region compiles exactly as the original file did.  Host fixtures use this
# to keep exercising one part of a consolidated unit.
set -eu

src=$1
shift

printf '/*\n * zedBSD\n * Copyright (C) 2026 Awe Morris\n *\n'
printf ' * SPDX-License-Identifier: Zlib\n */\n\n'
printf '/*\n * Extracted from %s by tools/kstyle/kregion.sh.  Do not edit.\n */\n\n' "$src"

for region in "$@"; do
	if [ "$(grep -c "^/\* Begin consolidated $region\. \*/$" "$src")" != 1 ]; then
		echo "kregion.sh: no unique region '$region' in $src" >&2
		exit 1
	fi
	sed -n "/^\/\* Begin consolidated $region\. \*\/$/,/^\/\* End consolidated $region\. \*\/$/p" "$src" |
		sed '1d;$d'
done

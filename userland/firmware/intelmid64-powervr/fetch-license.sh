#!/bin/sh
# zedBSD license fetch helper for Google AOSP
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
set -eu

output=
url=

while test $# -gt 0; do
	case "$1" in
	--output)
		output="$2"
		shift 2
		;;
	*)
		url="$1"
		shift
		;;
	esac
done

if test -z "$output" || test -z "$url"; then
	echo "fetch-license: usage: fetch-license.sh --output FILE URL" >&2
	exit 1
fi

if echo "$url" | grep -q 'format=TEXT'; then
	curl --fail --location --silent --show-error --retry 3 --retry-all-errors --retry-delay 5 "$url" | base64 -d > "$output"
else
	curl --fail --location --silent --show-error --retry 3 --retry-all-errors --retry-delay 5 --output "$output" "$url"
fi

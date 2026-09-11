#!/bin/sh
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib

# A deliberately small curl-shaped transport used only by the Noct lifecycle
# fixtures.  It never opens a network connection: the requested fixture URL is
# checked literally and bytes come only from the caller-provided local file.

set -eu

output=
url=
while test "$#" -gt 0; do
	case "$1" in
	--output)
		test "$#" -ge 2 || {
			echo "Noct fixture fetch: --output requires a path" >&2
			exit 2
		}
		output=$2
		shift 2
		;;
	--*)
		echo "Noct fixture fetch: unsupported option: $1" >&2
		exit 2
		;;
	*)
		test -z "$url" || {
			echo "Noct fixture fetch: multiple URLs are not supported" >&2
			exit 2
		}
		url=$1
		shift
		;;
	esac
done

test -n "$output" || {
	echo "Noct fixture fetch: output path is missing" >&2
	exit 2
}
test "$url" = "fixture://NoctFixture-1.0.tar.gz" || {
	echo "Noct fixture fetch: refusing non-fixture URL: $url" >&2
	exit 2
}

source=${ZEDBSD_NOCT_FIXTURE_FETCH_SOURCE-}
test -f "$source" && test ! -L "$source" || {
	echo "Noct fixture fetch: local source is missing or unsafe" >&2
	exit 2
}

case ${ZEDBSD_NOCT_FIXTURE_FETCH_MODE-copy} in
copy)
	cp "$source" "$output"
	;;
partial)
	dd if="$source" of="$output" bs=1 count=17 2>/dev/null
	echo "Noct fixture fetch: simulated interrupted transfer" >&2
	exit 73
	;;
*)
	echo "Noct fixture fetch: unknown mode" >&2
	exit 2
	;;
esac

#!/bin/sh
# Fetches the JVT conformance bitstreams of plan/ws326/tests/conformance/list.txt (ITU-T H.264.1) with
# parameter-free GETs from the ITU's public FTP mirror, and checks each zip against the SHA-256 recorded
# in plan/ws326/tests/conformance/sha256.txt (a zip with no line there is recorded on its first fetch;
# the caller commits the new lines).  A zip already in the cache with the recorded hash is not fetched again.
#   sh plan/ws326/tests/conformance/fetch.sh [<dir>/<name>...]     (default: every line of list.txt)
# The cache is $H264SW_CONFORMANCE_DIR (default build/tmp/ws326-conformance/zips); its zips are unpacked
# next to them in <name>/, without the reference decoder's trace files (about 9 parts in 10 of the unpacked size;
# the comparison does not read them: the 204 zips of 3.1 GB unpack to about 7 GB without them, 35 GB with).  An answer that is not a zip (an HTML page, a bot check) stops the run at once:
# report it, do not retry with another client or User-Agent.
# Last line: fetch: PASS (<n> zips) or fetch: FAIL.
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
set -u
cd "$(dirname -- "$0")/../../../.."
list=plan/ws326/tests/conformance/list.txt
hashes=plan/ws326/tests/conformance/sha256.txt
cache=${H264SW_CONFORMANCE_DIR:-build/tmp/ws326-conformance/zips}
base=https://www.itu.int/wftp3/av-arch/jvt-site/draft_conformance
mkdir -p "$cache"
[ -f "$hashes" ] || : > "$hashes"

names="$*"
if [ -z "$names" ]; then
	names=$(grep -v '^#' "$list" | cut -f1)
fi

count=0
for entry in $names; do
	name=$(basename "$entry")
	zip="$cache/$name.zip"
	recorded=$(awk -v n="$entry" '$2 == n { print $1 }' "$hashes")
	if [ -f "$zip" ] && [ -n "$recorded" ]; then
		actual=$(sha256sum "$zip" | cut -d' ' -f1)
		if [ "$actual" = "$recorded" ]; then
			count=$((count + 1))
			continue
		fi
	fi
	if ! timeout 900 curl -sS -f -o "$zip.part" "$base/$entry.zip"; then
		echo "FAILED fetching $entry"
		echo "fetch: FAIL"
		exit 1
	fi
	if ! unzip -tq "$zip.part" > /dev/null 2>&1; then
		echo "FAILED $entry is not a zip (stop: report it, do not retry another way)"
		echo "fetch: FAIL"
		exit 1
	fi
	mv "$zip.part" "$zip"
	actual=$(sha256sum "$zip" | cut -d' ' -f1)
	if [ -z "$recorded" ]; then
		printf '%s\t%s\n' "$actual" "$entry" >> "$hashes"
	elif [ "$actual" != "$recorded" ]; then
		echo "FAILED $entry: SHA-256 $actual, recorded $recorded"
		echo "fetch: FAIL"
		exit 1
	fi
	mkdir -p "$cache/$name"
	unzip -oq "$zip" -d "$cache/$name" -x '*[Tt][Rr][Aa][Cc][Ee]*' '*.[Tt][Rr][Cc]' 2> /dev/null
	count=$((count + 1))
done

echo "fetch: PASS ($count zips)"

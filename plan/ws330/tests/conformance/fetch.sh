#!/bin/sh
# Fetches the HEVC conformance bitstreams of plan/ws330/tests/conformance/list.txt (ITU-T H.265.1) with
# parameter-free GETs from the ITU's public FTP mirror, and checks each zip against the SHA-256 recorded
# in plan/ws330/tests/conformance/sha256.txt (a zip with no line there is recorded on its first fetch;
# the caller commits the new lines).  A zip already in the cache with the recorded hash is not fetched again.
#   sh plan/ws330/tests/conformance/fetch.sh [<name>...]     (default: every line of list.txt)
# The cache is $H265SW_CONFORMANCE_DIR (default build/tmp/ws330-conformance; the shared place is
# /home/awe/zedBSD-claude1/build/sources/h265-conformance, which Q1 makes).  The zips are not unpacked:
# the tools read the bitstream and the MD5 file with `unzip -p`, so the disk holds only the zips (129 MB).
# An answer that is not a zip (an HTML page, a bot check) stops the run at once: report it, do not retry
# with another client or User-Agent.
# Last line: fetch: PASS (<n> zips) or fetch: FAIL.
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
set -u
cd "$(dirname -- "$0")/../../../.."
list=plan/ws330/tests/conformance/list.txt
hashes=plan/ws330/tests/conformance/sha256.txt
cache=${H265SW_CONFORMANCE_DIR:-build/tmp/ws330-conformance}
base=https://www.itu.int/wftp3/av-arch/jctvc-site/bitstream_exchange/draft_conformance/HEVC_v1
mkdir -p "$cache"
[ -f "$hashes" ] || : > "$hashes"

names="$*"
if [ -z "$names" ]; then
	names=$(grep -v '^#' "$list" | cut -f1)
fi

count=0
for name in $names; do
	zip="$cache/$name.zip"
	recorded=$(awk -v n="$name" '$2 == n { print $1 }' "$hashes")
	if [ -f "$zip" ] && [ -n "$recorded" ]; then
		actual=$(sha256sum "$zip" | cut -d' ' -f1)
		if [ "$actual" = "$recorded" ]; then
			count=$((count + 1))
			continue
		fi
	fi
	part="$cache/$name.zip.part"
	if ! timeout 900 curl -sS -f -o "$part" "$base/$name.zip"; then
		echo "FAILED fetching $name"
		echo "fetch: FAIL"
		exit 1
	fi
	if ! unzip -tq "$part" > /dev/null 2>&1; then
		echo "FAILED $name is not a zip (stop: report it, do not retry another way)"
		echo "fetch: FAIL"
		exit 1
	fi
	mv "$part" "$zip"
	actual=$(sha256sum "$zip" | cut -d' ' -f1)
	if [ -z "$recorded" ]; then
		printf '%s\t%s\n' "$actual" "$name" >> "$hashes"
	elif [ "$actual" != "$recorded" ]; then
		echo "FAILED $name: SHA-256 $actual, recorded $recorded"
		echo "fetch: FAIL"
		exit 1
	fi
	count=$((count + 1))
done
echo "fetch: PASS ($count zips)"

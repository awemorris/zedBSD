#!/bin/sh
# ws001-p045: fetches the POSIX.1-2024 (Issue 8) header pages that extract.py reads, into a directory of their own.
#   plan/ws001/tests/posix-headers/fetch.sh build/ws001-posix-ref
# Then: python3 -I plan/ws001/tests/posix-headers/extract.py build/ws001-posix-ref/pages plan/ws001/tests/posix-headers/posix-2024.json
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
set -eu
base=https://pubs.opengroup.org/onlinepubs/9799919799
out=$1
mkdir -p "$out/pages"
curl -sf -o "$out/head.html" "$base/idx/head.html"
for page in $(grep -o 'basedefs/[a-z0-9_/]*\.h\.html' "$out/head.html" | sort -u); do
	curl -sf -o "$out/pages/$(basename "$page")" "$base/$page"
done
ls "$out/pages" | wc -l

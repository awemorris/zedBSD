#!/bin/sh
# ws056: builds the tree and the pax, gnu and ustar archives that
# guest-pax-test.sh extracts, and the host's expectations for them.
# Names exceed the ustar name (100) and prefix (155) fields and a link
# target exceeds 100 bytes, while every path stays under zedBSD's
# PATH_MAX (256) and no link target contains "..".
#   make-pax-archives.sh OUTPUT_DIR COREUTILS_SRC_TAR
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
set -e
out=$(mkdir -p "$1" && cd "$1" && pwd)
coreutils=$(cd "$(dirname "$2")" && pwd)/$(basename "$2")
rm -rf "$out/src" "$out"/*.tar "$out"/expect-*.txt
mkdir -p "$out/src"
cd "$out/src"
A=$(printf 'a%.0s' $(seq 1 60)); B=$(printf 'b%.0s' $(seq 1 60)); C=$(printf 'c%.0s' $(seq 1 40))
D=$(printf 'd%.0s' $(seq 1 50)); E=$(printf 'e%.0s' $(seq 1 50)); N=$(printf 'n%.0s' $(seq 1 110))
T=$(printf 't%.0s' $(seq 1 100))
mkdir -p "long/$A/$B/$C" "long/$D/$E" "target/$T"
echo prefix-too-long > "long/$A/$B/$C/short-name.txt"
echo name-too-long > "long/$D/$E/$N"
echo short > long/short.txt
ln long/short.txt long/hardlink.txt
echo tgt > "target/$T/file.txt"
ln -s "target/$T/file.txt" symlink-long
ln -s short.txt long/symlink-short
mkdir -p plain/sub
printf 'a\nb\n' > plain/sub/x
touch -d '2020-02-02 02:02:02.123456789' plain/sub/x
mkfifo plain/fifo
mkdir -p "prefixonly/$A/$B"
echo p > "prefixonly/$A/$B/name-fits-in-100-bytes.txt"
tar xf "$coreutils" --wildcards 'coreutils-9.12/tests/*' 'coreutils-9.12/src/*' 'coreutils-9.12/README*'
cd "$out"
tar --format=pax -cf pax.tar -C src .
tar --format=gnu -cf gnu.tar -C src .
tar --format=ustar -cf ustar.tar -C src plain prefixonly
(cd src && find . -type f -exec cksum {} + | sort) > expect-files.txt
(cd src && find . -type l | sort | while read l; do printf '%s -> %s\n' "$l" "$(readlink "$l")"; done) > expect-links.txt
(cd src && find . -type d | sort) > expect-dirs.txt
wc -l expect-files.txt expect-links.txt expect-dirs.txt

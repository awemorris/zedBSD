#!/bin/sh
# The host test of mkfs's native UFS profiles: the 1 KiB-fragment profiles (ordinary, feature and native on
# 512-byte sectors) are byte for byte what the formatter of OLD (default d94ceac00, before the 4 KiB-fragment
# profile) writes -- except that native no longer writes the tail journal's and the snapshot's records after the
# volume, which are cleared from OLD's output before the comparison -- and the native profile on 512- and
# 4096-byte sectors verifies, decodes, has no tail record and mounts with the loaders' reader.  Output goes to a new build/installer-test/mkfs-ufs.* directory (sparse files, a few MB on disk).
#   sh plan/tools/installer/host/mkfs-ufs-native-test.sh [OLD]
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
set -eu
cd "$(dirname "$0")/../../../.."
old=${1:-d94ceac00}
mkdir -p build/installer-test
out=$(mktemp -d build/installer-test/mkfs-ufs.XXXXXX)
mkdir "$out/old"
git show "$old:userland/base/mkfs/ufs-format.c" > "$out/old/ufs-format.c"
git show "$old:userland/base/mkfs/ufs-format.h" > "$out/old/ufs-format.h"
srcs="userland/base/mkfs/ufs-super.c userland/base/mkfs/ufs-endian.c bootloader/common/ufs-read.c"
cc -std=c99 -D_DEFAULT_SOURCE -Wall -Wextra -Werror -I. -Iuserland/base/mkfs -o "$out/new" \
	plan/tools/installer/host/mkfs-ufs-native-test.c userland/base/mkfs/ufs-format.c $srcs
cc -std=c99 -D_DEFAULT_SOURCE -DOLD_API -Wall -Wextra -Werror -Wno-unused-function -Wno-unused-variable -I. -I"$out/old" -Iuserland/base/mkfs -o "$out/old/old" \
	plan/tools/installer/host/mkfs-ufs-native-test.c "$out/old/ufs-format.c" $srcs
fail=0
for case in ordinary:4194304 ordinary:67108864 ordinary:2147482624 feature:4194304 feature:268435456 \
	native:4194304 native:67111936 native:1073741824 native:5368709120 native:68719480832; do
	profile=${case%%:*}
	bytes=${case#*:}
	"$out/old/old" write "$profile" 512 "$bytes" "$out/a.img"
	"$out/new" write "$profile" 512 "$bytes" "$out/b.img"
	[ "$profile" = native ] && python3 -I -c '
import sys
path = sys.argv[1]
with open(path, "r+b") as f:
	f.seek(0, 2)
	size = f.tell()
	offset = max(0, size - (3 << 20))
	while offset + 512 <= size:
		f.seek(offset)
		if f.read(4) in (b"ZUJ2", b"ZSL1", b"ZSN1"):
			f.seek(offset)
			f.write(bytes(512))
		offset += 512
' "$out/a.img"
	if cmp -s "$out/a.img" "$out/b.img"; then
		echo "same $profile $bytes"
	else
		echo "DIFFERS $profile $bytes"
		fail=1
	fi
done
for case in 512:4194304 512:1073741824 4096:4194304 4096:67108864 4096:268431360 4096:1073741824 \
	4096:17179897856 4096:107374182400; do
	"$out/new" check "${case%%:*}" "${case#*:}" "$out/c.img" || fail=1
done
if [ $fail -ne 0 ]; then
	echo "mkfs-ufs-native-test: FAIL $out"
	exit 1
fi
echo "mkfs-ufs-native-test: PASS $out"

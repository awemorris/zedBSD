#!/bin/sh
# The host test of the loaders' UFS reader: a UFS volume made by zedimage-host from a tree with a large kernel
# (past the single indirect block), a logo, a deep directory, a large directory, an empty file and a symbolic link,
# read back through bootloader/common/ufs-read.c.  Output goes to a new build/installer-test/ufs-read.* directory.
#   sh plan/tools/installer/host/ufs-read-test.sh
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
set -eu
cd "$(dirname "$0")/../../../.."
make -s ZEDBSD_CONFIG=config/ci/config-amd64.mk build/zedimage-host
mkdir -p build/installer-test
out=$(mktemp -d build/installer-test/ufs-read.XXXXXX)
tree=$out/tree
mkdir -p "$tree/boot" "$tree/etc" "$tree/a/b/c/d/e/f/g/h" "$tree/many"
head -c 25165824 /dev/urandom > "$tree/boot/vmunix"
head -c 3000000 /dev/urandom > "$tree/boot/logo.ppm"
printf 'hostname="zedbsd"\n' > "$tree/etc/rc.conf"
printf 'deep\n' > "$tree/a/b/c/d/e/f/g/h/deep.txt"
: > "$tree/empty"
i=0
while [ $i -lt 400 ]; do
	printf '%s\n' "$i" > "$tree/many/$(printf 'file-%04d' $i)"
	i=$((i + 1))
done
ln -s boot "$tree/link"
build/zedimage-host ufs 67108864 "$tree" "$out/root.ufs" > "$out/zedimage.txt" 2>&1
cc -std=c99 -Wall -Wextra -Werror -I. -o "$out/ufs-read-test" plan/tools/installer/host/ufs-read-test.c bootloader/common/ufs-read.c
"$out/ufs-read-test" "$out/root.ufs" "$tree"

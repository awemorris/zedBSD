#!/bin/sh
# Install-admin's index and unpacking on the host.  Builds plan/tools/installer/host/install-admin-unpack.c with
# the helper's index.c and unpack.c, libz-compat's inflate and the common SHA-256, then: a good unit (files, a
# set-user-ID file the index allows, a symbolic link, a 1777 directory) and the refusals (a set-ID bit the index does not
# allow, an absolute path, "..", a write through a symbolic link an earlier entry made, a hard link, a pax header, a
# damaged header, another SHA-256, another size, a damaged CRC-32, a file that is already there), the dependency order
# and a cycle, the index's own digest, and, when INSTALLER_PAYLOAD names a built payload (its index and units/), every
# unit of it unpacked with its entry count checked against the index.  The last line is "install-admin-unpack-test:
# PASS|FAIL".  Output goes to a new build/tmp/install-admin-unpack.*.
#   sh plan/tools/installer/host/install-admin-unpack-test.sh
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
set -u
cd "$(dirname "$0")/../../../.."
mkdir -p build/tmp
out=$(mktemp -d build/tmp/install-admin-unpack.XXXXXX)
mkdir -p "$out/inc"
ln -s "$(pwd)/include/libc/compat" "$out/inc/compat"
status=0
cc -std=c11 -D_DEFAULT_SOURCE -D_GNU_SOURCE -O1 -g -Wall -Wextra -Werror -fsanitize=address,undefined -I. -I"$out/inc" \
	-o "$out/unpack" plan/tools/installer/host/install-admin-unpack.c userland/desktop/installer/admin/index.c \
	userland/desktop/installer/admin/unpack.c userland/base/common/sha256.c userland/base/libz-compat/inflate.c \
	userland/base/libz-compat/checksum.c || { echo "install-admin-unpack-test: FAIL (build)"; exit 1; }
python3 -I plan/tools/installer/host/install-admin-fixtures.py "$out" > /dev/null
check() {
	if [ "$2" = "$3" ]; then echo "ok $1"; else echo "fail $1: got '$2' want '$3'"; status=1; fi
}
run() { case=$1; mkdir -p "$out/root-$case"; "$out/unpack" unpack "$out/$case.index" "$case" "$out/$case.tar.gz" "$out/root-$case"; }
check good "$(run good)" "RESULT 0 6"
check good-mode "$(stat -c %a "$out/root-good/usr/bin/su") $(stat -c %a "$out/root-good/tmp") $(readlink "$out/root-good/usr/bin/link")" "4755 1777 tool"
check good-again "$("$out/unpack" unpack "$out/good.index" good "$out/good.tar.gz" "$out/root-good")" "RESULT 17 2"
check setid "$(run setid)" "RESULT 1 3"
check absolute "$(run absolute)" "RESULT 22 0"
check dotdot "$(run dotdot)" "RESULT 22 1"
result=$(run escape)
case $result in "RESULT 40 2"|"RESULT 20 2") echo "ok escape";; *) echo "fail escape: $result"; status=1;; esac
[ -e "$out/outside" ] && { echo "fail escape wrote outside"; status=1; }
check hardlink "$(run hardlink)" "RESULT 22 1"
check pax "$(run pax)" "RESULT 22 0"
check checksum "$(run checksum)" "RESULT 22 0"
check digest "$(run digest)" "RESULT 74 read"
check size "$(run size)" "RESULT 5 read"
check crc "$(run crc)" "RESULT 22 gunzip"
check closure "$("$out/unpack" closure "$out/deps.index" d)" "ORDER c b a d"
check closure2 "$("$out/unpack" closure "$out/deps.index" a c)" "ORDER c b a"
check cycle "$("$out/unpack" closure "$out/deps.index" x)" "RESULT 40"
check bad-format "$("$out/unpack" closure "$out/bad-format.index" a)" "RESULT 22 parse"
check unknown-dep "$("$out/unpack" closure "$out/unknown-dep.index" a)" "RESULT 22 parse"
check unknown-record "$("$out/unpack" closure "$out/unknown-record.index" a)" "RESULT 22 parse"
check digest-match "$("$out/unpack" digest "$out/good.index" "$(sha256sum "$out/good.index" | cut -d' ' -f1)")" "RESULT 0"
check digest-other "$("$out/unpack" digest "$out/good.index" "$(sha256sum "$out/setid.index" | cut -d' ' -f1)")" "RESULT 74"
if [ -n "${INSTALLER_PAYLOAD:-}" ]; then
	mkdir -p "$out/payload-root"
	for unit in $(awk -F'\t' '$1 == "unit" {print $2}' "$INSTALLER_PAYLOAD/index"); do
		want=$(awk -F'\t' -v u="$unit" '$1 == "unit" && $2 == u {print $8}' "$INSTALLER_PAYLOAD/index")
		check "payload-$unit" "$("$out/unpack" unpack "$INSTALLER_PAYLOAD/index" "$unit" "$INSTALLER_PAYLOAD/units/$unit.tar.gz" "$out/payload-root")" "RESULT 0 $want"
	done
fi
[ $status -eq 0 ] && echo "install-admin-unpack-test: PASS $out" || echo "install-admin-unpack-test: FAIL $out"
exit $status

#!/bin/sh
# ws043: runs real configure scripts twice, once with the host's GNU tools
# and once with zedBSD's utilities (userland/base built for the host) first
# on PATH and as AWK, SED and GREP, and compares what they generate.
#
#   sh plan/tools/utils/configure-diff.sh [BIN_DIR] [PACKAGE...]
#
# BIN_DIR defaults to build/ws043/bin (see build-host-utils.sh).  PACKAGE
# names a tarball in build/distfiles without .tar.xz; the default is
# expat-2.8.5 and coreutils-9.12.  Paths that differ only because of the
# two trees or the two tool locations are made the same before comparing;
# config.log is not compared.  Exits 1 when any package differs.
set -eu
root=$(pwd)
bin=$(cd "${1:-build/ws043/bin}" && pwd)
[ $# -gt 0 ] && shift
packages=${*:-expat-2.8.5 coreutils-9.12}
work="$root/build/ws043/configure"
status=0
for package in $packages; do
	rm -rf "$work/$package"
	for side in gnu ours; do
		mkdir -p "$work/$package/$side"
		tar -xJf "build/distfiles/$package.tar.xz" -C "$work/$package/$side"
		if [ "$side" = gnu ]; then
			(cd "$work/$package/$side/$package" &&
				AWK=/usr/bin/gawk SED=/usr/bin/sed GREP=/usr/bin/grep \
				./configure >"$work/$package/$side.log" 2>&1) ||
				echo "$package: configure failed with the GNU tools"
		else
			(cd "$work/$package/$side/$package" &&
				PATH="$bin:$PATH" AWK="$bin/awk" SED="$bin/sed" GREP="$bin/grep" \
				./configure >"$work/$package/$side.log" 2>&1) ||
				echo "$package: configure failed with zedBSD's tools"
		fi
	done
	# The same names for the trees and the tools, in every file that differs.
	(cd "$work/$package" &&
		diff -rq gnu ours 2>/dev/null | sed -n 's/^Files ours\/\(.*\) and .*/\1/p; s/^Files gnu\/\([^ ]*\) and ours.*/\1/p' |
		while read -r file; do
			for side in gnu ours; do
				sed -i -e "s|$work/$package/$side|TREE|g" \
					-e "s|$bin/awk|/usr/bin/gawk|g" \
					-e "s|$bin/sed|/usr/bin/sed|g" \
					-e "s|$bin/grep|/usr/bin/grep|g" \
					-e "s|$bin:||g" "$side/$file"
			done
		done)
	if (cd "$work/$package" && diff -r -x config.log gnu ours >"$work/$package.diff"); then
		echo "$package: same ($(cd "$work/$package/ours/$package" && ls config.status 2>/dev/null))"
	else
		echo "$package: DIFFERENT, see $work/$package.diff"
		status=1
	fi
done
exit $status

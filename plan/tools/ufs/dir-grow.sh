#!/bin/sh
# ws054: grows UFS directories past one block on a mounted volume, then
# checks what it made (again after a remount).
#   sh dir-grow.sh DIR make     builds the directories and prints LIMIT n
#   sh dir-grow.sh DIR verify   checks every name the make step left
# LONG, SHORT, MOVE and GONE set the counts (defaults 1500, 3000, 500, 1500;
# the journal's volume is tested with smaller ones, its operations are slow).
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
set -u
top=$1
long=file-with-a-rather-long-name-number
nlong=${LONG:-1500}
nshort=${SHORT:-3000}
nmove=${MOVE:-500}
ngone=${GONE:-1500}

fail() {
	echo "FAIL $*"
	exit 1
}

# Notes a step on the console as well, where it can still be seen when the
# machine stops answering (set PROGRESS=1).
progress() {
	if [ "${PROGRESS:-0}" = 1 ]; then
		echo "dir-grow: $*" > /dev/console
	fi
}

make_all() {
	mkdir -p "$top/long" "$top/short" "$top/moved" || fail mkdir

	# 1500 long names (10 of the 12 blocks a directory may have) and 3000
	# short ones (6 blocks).
	i=0
	while [ $i -lt "$nshort" ] || [ $i -lt "$nlong" ]; do
		if [ $i -lt "$nlong" ]; then
			: > "$top/long/$long-$i" || fail "create long $i"
		fi
		if [ $i -lt "$nshort" ]; then
			: > "$top/short/s$i" || fail "create short $i"
		fi
		i=$((i + 1))
		[ $((i % 250)) -eq 0 ] && progress "created $i"
	done

	progress "removing long names"
	# Every other long name goes, and comes back under another name.
	i=0
	while [ $i -lt "$nlong" ]; do
		rm "$top/long/$long-$i" || fail "remove long $i"
		i=$((i + 2))
	done
	i=0
	while [ $i -lt "$nlong" ]; do
		: > "$top/long/again-$i" || fail "create again $i"
		i=$((i + 2))
	done

	progress "moving short names"
	# 500 short names move to another directory.
	i=0
	while [ $i -lt "$nmove" ]; do
		mv "$top/short/s$i" "$top/moved/m$i" || fail "move $i"
		i=$((i + 1))
	done

	progress "gone"
	# A directory filled past one block, emptied and removed.
	mkdir "$top/gone" || fail "mkdir gone"
	i=0
	while [ $i -lt "$ngone" ]; do
		: > "$top/gone/g$i" || fail "create gone $i"
		i=$((i + 1))
	done
	i=0
	while [ $i -lt "$ngone" ]; do
		rm "$top/gone/g$i" || fail "empty gone $i"
		i=$((i + 1))
	done
	rmdir "$top/gone" || fail "rmdir gone"

	# A directory filled until the volume refuses the next name.
	progress "full"
	mkdir "$top/full" || fail "mkdir full"
	i=0
	while [ $i -lt 20000 ]; do
		true > "$top/full/limit-entry-$i" 2>/dev/null || break
		i=$((i + 1))
		[ $((i % 250)) -eq 0 ] && progress "full $i"
	done
	echo "LIMIT $i"
	true > "$top/full/limit-entry-$i"
	echo "REFUSED status=$?"
}

verify_all() {
	[ "$(ls "$top/long" | wc -l)" -eq "$nlong" ] || fail "long count $(ls "$top/long" | wc -l)"
	[ "$(ls "$top/short" | wc -l)" -eq $((nshort - nmove)) ] || fail "short count $(ls "$top/short" | wc -l)"
	[ "$(ls "$top/moved" | wc -l)" -eq "$nmove" ] || fail "moved count $(ls "$top/moved" | wc -l)"
	[ ! -e "$top/gone" ] || fail "gone exists"
	i=0
	while [ $i -lt "$nshort" ] || [ $i -lt "$nlong" ]; do
		if [ $i -ge "$nlong" ]; then
			:
		elif [ $((i % 2)) -eq 0 ]; then
			[ -e "$top/long/again-$i" ] || fail "missing again-$i"
			[ ! -e "$top/long/$long-$i" ] || fail "stale $long-$i"
		else
			[ -e "$top/long/$long-$i" ] || fail "missing $long-$i"
		fi
		if [ $i -lt "$nmove" ]; then
			[ -e "$top/moved/m$i" ] || fail "missing m$i"
			[ ! -e "$top/short/s$i" ] || fail "stale s$i"
		elif [ $i -lt "$nshort" ]; then
			[ -e "$top/short/s$i" ] || fail "missing s$i"
		fi
		i=$((i + 1))
	done
	echo "FULL $(ls "$top/full" | wc -l)"
	echo VERIFY-OK
}

case $2 in
make) make_all ;;
verify) verify_all ;;
*) fail "usage: dir-grow.sh DIR make|verify" ;;
esac

#!/bin/sh
# ws054: creates names in one directory without end, printing each one it
# made, so the host can stop the machine in the middle of a directory's
# growth.  After the restart, `check` counts what the directory holds.
#   sh crash-grow.sh DIR run      creates DIR/c/n0, n1, ... until stopped
#   sh crash-grow.sh DIR check    prints how many names DIR/c holds
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
set -u
top=$1

case $2 in
run)
	mkdir -p "$top/c" || exit 1
	i=0
	while true > "$top/c/crash-entry-name-$i"; do
		echo "MADE $i"
		i=$((i + 1))
	done
	;;
check)
	echo "HOLDS $(ls "$top/c" | wc -l)"
	i=0
	while [ -e "$top/c/crash-entry-name-$i" ]; do
		i=$((i + 1))
	done
	echo "PREFIX $i"
	;;
esac

#!/bin/sh
# WS049: loads tables through a simulated firmware memory (RSDP, root
# table, FADT) and compares the namespace with loading the files directly.
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
#
#   plan/tools/aml-host/compare-firmware.sh [--rsdt] NAME dsdt.dat [TABLE.dat...]
#
# The tables are laid out by make-firmware.py (an XSDT, or with --rsdt an
# RSDT); the SSDTs load in the order given.  Exits 0 when the namespaces
# are the same.
set -eu
repo=$(cd "$(dirname "$0")/../../.." && pwd)
root=""
if [ "$1" = "--rsdt" ]; then
	root="--rsdt"
	shift
fi
name=$1
shift
out="$repo/build/aml-host/firmware/$name"
host="$repo/build/aml-host/aml-host"
python3 "$repo/plan/tools/aml-host/make-firmware.py" $root "$out" "$@" > /dev/null
aml=""
for table in "$@"; do
	case "$(basename "$table")" in
	dsdt.dat|ssdt*.dat) aml="$aml $table" ;;
	esac
done
"$host" --quiet --dump --firmware "$out/memory.txt" > "$out.firmware.txt"
"$host" --quiet --dump $aml > "$out.files.txt"
if diff "$out.files.txt" "$out.firmware.txt" > "$out.diff"; then
	echo "$name: same ($(wc -l < "$out.firmware.txt") nodes)"
	exit 0
fi
echo "$name: DIFFERENT, see $out.diff"
exit 1

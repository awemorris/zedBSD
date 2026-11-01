#!/bin/sh
# The host check of libmedia's H.264 reader and DPB on the JVT streams (ws322-p007b): libmedia-host writes what the
# Wave5 engine gets for each picture libmedia decodes, ffmpeg decodes it, and its frames are compared in order with
# the reference (make-jvt.sh's .sha256).  One line per stream: the pictures libmedia decoded and dropped, the frames
# and how many equal the reference in order.
#   sh plan/ws322/tests/p007b/libmedia-check.sh LIBMEDIA-HOST JVT OUT [NAME...]
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
set -u
tool=$1
jvt=$2
out=$3
shift 3
mkdir -p "$out"
names=$*
[ -n "$names" ] || names=$(cd "$jvt" && ls *.h264 | sed 's/\.h264$//')
for n in $names; do
	timeout 300 "$tool" "$jvt/$n.h264" "$out/$n.ring.h264" > "$out/$n.txt" 2>&1
	timeout 600 ffmpeg -hide_banner -loglevel error -i "$out/$n.ring.h264" -pix_fmt nv12 -f framehash -hash sha256 - 2> /dev/null |
		grep -v '^#' | awk -F', *' '{ print $6 }' > "$out/$n.got"
	same=$(paste -d' ' "$out/$n.got" "$jvt/$n.sha256" | awk '$1 == $2 { c++ } END { print c + 0 }')
	echo "$n decoded $(grep -c ' decode 1 ' "$out/$n.txt") dropped $(grep -c ' decode 0 ' "$out/$n.txt") frames $(grep -c . "$out/$n.got") of $(grep -c . "$jvt/$n.sha256") equal $same | $(grep STOP "$out/$n.txt")"
done

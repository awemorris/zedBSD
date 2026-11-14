#!/bin/sh
# Record of the WS338 p001 flag comparison (paths are those of that run).
# Builds speed-bench with CC and CFLAGS and times two streams (3 runs, best fps).
# usage: flags.sh TAG CC "CFLAGS" STREAM...
set -u
tag=$1; cc=$2; cflags=$3; shift 3
root=/home/awe/zedBSD-worktrees/p1-ws338
out=/tmp/claude-1000/p1-ws338/flags/$tag
mkdir -p "$out"
cd "$root" || exit 1
objs=""
for f in userland/desktop/libmedia/h264sw-*.c userland/desktop/libmedia/vcodec-*.c userland/desktop/libmedia/bits.c plan/tools/h264sw/speed/speed-bench.c; do
	case $f in */h264sw-backend.c) continue ;; esac
	o=$out/$(basename "$f" .c).o
	# shellcheck disable=SC2086
	timeout 300 $cc -std=c89 $cflags -D_DEFAULT_SOURCE -I. -c "$f" -o "$o" || { echo "$tag: build FAIL $f"; exit 1; }
	objs="$objs $o"
done
# shellcheck disable=SC2086
$cc $objs -lm -o "$out/bench" || { echo "$tag: link FAIL"; exit 1; }
for s in "$@"; do
	r=$(timeout 1800 "$out/bench" "$s" 3 2>&1 | tail -1)
	echo "$tag $(basename "$s" .h264): $r"
done

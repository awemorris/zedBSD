#!/bin/sh
# ws122-p003: the host test of the media file reader (userland/desktop/mediafile).  make-media.py makes MP4 and
# Matroska files whose contents are known and what host-mediafile must print for each; host-mediafile.c (with the
# reader, ASan and UBSan) prints what the reader finds; the two must be the same.  Then the player's test file
# sample.mp4 (MPEG-4 Part 2 25 fps and AAC, 20 s) is read: two tracks, 500 video packets, every packet read.
# Last line: host-mediafile: PASS.
#   sh plan/tools/media/run-host-mediafile.sh
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
set -u
cd "$(dirname -- "$0")/../../.."
. plan/tools/fresh-out.sh
fresh_out build/ws122-host
out=build/ws122-host
mkdir -p "$out/media"
status=0
cc -std=gnu89 -O1 -g -Wall -Wextra -Werror -D_GNU_SOURCE -fsanitize=address,undefined -fno-omit-frame-pointer -I. \
    plan/tools/media/host-mediafile.c userland/desktop/mediafile/mediafile.c userland/desktop/mediafile/mp4.c \
    userland/desktop/mediafile/mkv.c userland/desktop/mediafile/ts.c userland/desktop/mediafile/ogg.c userland/desktop/mediafile/avi.c -o "$out/host-mediafile" || { echo "host-mediafile: FAIL (build)"; exit 1; }
python3 plan/tools/media/make-media.py "$out/media" || { echo "host-mediafile: FAIL (make-media)"; exit 1; }

# Each made file against what it was made with.
for expected in "$out"/media/*.expected; do
	name=$(basename "$expected" .expected)
	file=$(ls "$out/media/$name".* | grep -v '\.expected$\|\.args$\|\.got$' | head -1)
	UBSAN_OPTIONS=halt_on_error=1 timeout 30 "$out/host-mediafile" "$file" $(cat "$out/media/$name.args") \
	    > "$out/media/$name.got" 2>&1
	if cmp -s "$expected" "$out/media/$name.got"; then
		echo "$name: ok"
	else
		echo "$name: FAILED"
		diff "$expected" "$out/media/$name.got" | head -10
		status=1
	fi
done

# The player's sample: two tracks, MPEG-4 Part 2 and AAC, 500 video packets, every packet read to the end.
UBSAN_OPTIONS=halt_on_error=1 timeout 30 "$out/host-mediafile" plan/tools/media/sample.mp4 10000000 > "$out/sample.got" 2>&1
video=$(grep -c '^PACKET track=0 ' "$out/sample.got")
audio=$(grep -c '^PACKET track=1 ' "$out/sample.got")
if grep -q '^TRACK 0 kind=1 codec=6 name=mp4v width=320 height=240' "$out/sample.got" &&
    grep -q '^TRACK 1 kind=2 codec=7 name=mp4a .*rate=48000 channels=2' "$out/sample.got" &&
    [ "$video" = 500 ] && [ "$audio" -gt 900 ] && grep -q "^END error=$(python3 -c 'import errno; print(errno.ENODATA)') " "$out/sample.got" &&
    grep -q '^AFTER track=0 pts=10000000 .* key=1 ' "$out/sample.got"; then
	echo "sample.mp4: ok (video $video, audio $audio)"
else
	echo "sample.mp4: FAILED (video $video, audio $audio)"
	head -5 "$out/sample.got"
	status=1
fi

# ws121-p002: the same file read through a source (mf_open_source) prints the same.
HOST_MEDIAFILE_SOURCE=1 UBSAN_OPTIONS=halt_on_error=1 timeout 30 "$out/host-mediafile" plan/tools/media/sample.mp4 10000000 \
    > "$out/sample-source.got" 2>&1
if cmp -s "$out/sample.got" "$out/sample-source.got"; then
	echo "sample.mp4 through a source: ok"
else
	echo "sample.mp4 through a source: FAILED"
	diff "$out/sample.got" "$out/sample-source.got" | head -5
	status=1
fi

[ $status = 0 ] && echo "host-mediafile: PASS" || echo "host-mediafile: FAIL"
exit $status

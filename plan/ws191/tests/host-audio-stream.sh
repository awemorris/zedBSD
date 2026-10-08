#!/bin/sh
# ws191-p002: builds and runs the host test of the sound's playback streams under ASan and UBSan:
#   1. both ends: libkeiland's kl_audio_stream_* over the tree's libwayland (built here from its sources), against the
#      compositor's audio-stream.c with a stand-in event loop and backend (host-audio-client.c, host-audio-server.c);
#   2. zedBSD's backend (audio-stream-zedbsd.c) against a stand-in audiod (host-audiod.c).
#   sh plan/ws191/tests/host-audio-stream.sh
# The objects go to a new directory under build/tmp (nothing is removed here; Q1's plan/tools/q1-clean.sh removes old runs).
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
set -eu
repo=$(CDPATH= cd -- "$(dirname -- "$0")/../../.." && pwd)
cd "$repo"
. plan/tools/fresh-out.sh
fresh_out "$repo/build/tmp/ws191-host-audio"
work=$fresh_dir
cc=${CC:-cc}
san='-fsanitize=address,undefined -fno-sanitize-recover=all -fno-omit-frame-pointer'
U=userland/desktop

# The client side: the tree's headers (wayland-client.h, keiland/keiland.h) and libwayland.
client_flags="-std=gnu17 -D_GNU_SOURCE -O1 -g -Wall -Wextra -Werror $san -I. -I$U/include -I$U/libkeiland/audio -Iplan/ws191/tests"
wayland=$(sed -n 's#^.*\(userland/desktop/libwayland/[a-z-]*\.c\).*$#\1#p' $U/libwayland/Makefile.linux)
for file in $wayland $U/libkeiland/audio/audio.c $U/libkeiland/audio/audio-protocol.c plan/ws191/tests/host-audio-client.c; do
	$cc $client_flags -c "$file" -o "$work/client-$(basename "$file" .c).o"
done

# The server side: the compositor's headers, as plan/ws131/tests/host-system.sh builds them.
mkdir -p "$work/include/keiland"
cp $U/include/keiland/keiland.h "$work/include/keiland/keiland.h"
server_flags="-std=gnu99 -D_GNU_SOURCE -O1 -g -Wall -Wextra -Werror -pthread $san -I. -I$work/include -Iplan/ws191/tests"
for file in $U/wayland/audio-stream.c $U/libkeiland-backend-linux/peer-linux.c plan/ws191/tests/host-audio-server.c; do
	$cc $server_flags -c "$file" -o "$work/server-$(basename "$file" .c).o"
done
$cc $san -pthread -o "$work/host-audio-stream" "$work"/client-*.o "$work"/server-*.o
HOST_AUDIO_DIR=$work timeout 60 "$work/host-audio-stream"

# zedBSD's backend against a stand-in audiod, on a socket in the work directory.
$cc -std=gnu17 -D_GNU_SOURCE -O1 -g -Wall -Wextra -Werror -pthread $san -I. \
	-DAUDIO_SOCKET_PATH="\"$work/audiod.sock\"" -DHOST_AUDIOD_PATH="\"$work/audiod.sock\"" \
	$U/libkeiland-backend-zedbsd/audio-stream-zedbsd.c plan/ws191/tests/host-audiod.c -o "$work/host-audiod"
timeout 60 "$work/host-audiod"
echo "WS191 p002 host audio stream tests PASS (ASan/UBSan)"

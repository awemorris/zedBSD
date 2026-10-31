#!/bin/sh
# Host tests of libmedia's software H.264 back end and the decoder layer's switching:
#   test-backend: h264sw-backend.c against a scripted stand-in for the decoder (every other h264sw-*.c and the vcodec-*.c real).
#   test-switch:  decoder.c against scripted stand-in back ends (open order, refusal before and after a packet was taken,
#                 draining, dropping to a key frame, the end before a key frame).
# ASan and UBSan; the last line is "run-backend: PASS" or "run-backend: FAIL".
#   sh plan/tools/h264sw/backend/run-backend.sh
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
set -u
cd "$(dirname -- "$0")/../../../.."
. plan/tools/fresh-out.sh
out=build/tmp/h264sw-backend
fresh_out "$out"

lib=userland/desktop/libmedia
modules=""
for file in $lib/h264sw-*.c $lib/vcodec-*.c; do
	case "$file" in
	*/h264sw-backend.c|*/h264sw-decoder.c) ;;
	*) modules="$modules $file" ;;
	esac
done
flags="-std=c89 -O1 -g -Wall -Wextra -Werror -D_DEFAULT_SOURCE -fsanitize=address,undefined -fno-omit-frame-pointer -I. -Iuserland/desktop/libmedia -Iplan/tools/h264sw/unit -Iuserland/desktop/include"

status=0
if cc $flags plan/tools/h264sw/backend/test-backend.c $lib/h264sw-backend.c $lib/bitstream.c $lib/picture.c $lib/bits.c $modules \
    -lm -lpthread -o "$out/test-backend" > "$out/test-backend.build.log" 2>&1; then
	if ASAN_OPTIONS=detect_leaks=1 UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1 timeout 300 "$out/test-backend" > "$out/test-backend.log" 2>&1; then
		tail -1 "$out/test-backend.log"
	else
		echo "test-backend: FAIL (exit; log $out/test-backend.log)"
		grep -v '^ok ' "$out/test-backend.log" | head -20
		status=1
	fi
else
	echo "test-backend: FAIL (build)"
	head -20 "$out/test-backend.build.log"
	status=1
fi

if cc $flags plan/tools/h264sw/backend/test-switch.c $lib/decoder.c -lpthread -o "$out/test-switch" > "$out/test-switch.build.log" 2>&1; then
	if ASAN_OPTIONS=detect_leaks=1 UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1 timeout 300 "$out/test-switch" > "$out/test-switch.log" 2>&1; then
		tail -1 "$out/test-switch.log"
	else
		echo "test-switch: FAIL (exit; log $out/test-switch.log)"
		grep -v '^ok ' "$out/test-switch.log" | head -20
		status=1
	fi
else
	echo "test-switch: FAIL (build)"
	head -20 "$out/test-switch.build.log"
	status=1
fi

if [ "$status" = 0 ]; then echo "run-backend: PASS"; else echo "run-backend: FAIL"; fi
exit "$status"

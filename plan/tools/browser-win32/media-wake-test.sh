#!/bin/sh
# Builds and runs plan/tools/browser-win32/media-wake-test.c over libmedia's objects of the WS074 host build
# (sh plan/ws074/tests/host-build.sh [plain|asan] first) on plan/tools/media/sample.mp4.
#   sh plan/tools/browser-win32/media-wake-test.sh [plain|asan]
# The output goes to build/tmp/browser-win32-media-wake/<variant>/.  Last line: media-wake-test: PASS, or the failures.
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
set -u
cd "$(dirname -- "$0")/../../.."
variant=${1:-plain}
case $variant in
plain) flags= ;;
asan) flags="-fsanitize=address,undefined -fno-omit-frame-pointer -fno-sanitize-recover=undefined" ;;
*) echo "media-wake-test: unknown variant $variant" >&2; exit 2 ;;
esac
objects=$(ls build/ws074-host/$variant/obj/media-*.o 2> /dev/null)
if [ -z "$objects" ]; then
	echo "media-wake-test: no libmedia objects in build/ws074-host/$variant/obj (run host-build.sh $variant)"
	exit 1
fi
out=build/tmp/browser-win32-media-wake/$variant
mkdir -p "$out"
cc $flags -std=gnu11 -O1 -g -Wall -Wextra -Werror -D_GNU_SOURCE -I. plan/tools/browser-win32/media-wake-test.c $objects \
	-o "$out/media-wake-test" -lvulkan -lm -ldl -lpthread || exit 1
timeout 60 "$out/media-wake-test" plan/tools/media/sample.mp4

#!/bin/sh
set -eu

repo=$(CDPATH= cd -- "$(dirname -- "$0")/../../.." && pwd)
temporary=${TMPDIR:-$repo/build/q047-tmp}/usb-storage-no-media
ordinary=$temporary/ordinary
sanitized=$temporary/sanitized
analyzed=$temporary/analyzed

mkdir -p "$temporary"

cc -ffunction-sections -fdata-sections -Wl,--gc-sections -std=c11 -O2 -Wall -Wextra -Werror \
	-I"$repo/include" -I"$repo/include/uapi" -I"$repo/src" -I"$repo" \
	"$repo/plan/ws004/tests/usb-storage-no-media-test.c" \
	"$repo/src/kern/io.c" -o "$ordinary"
"$ordinary"

cc -ffunction-sections -fdata-sections -Wl,--gc-sections -std=c11 -O1 -g -Wall -Wextra -Werror \
	-fsanitize=address,undefined -fno-omit-frame-pointer \
	-I"$repo/include" -I"$repo/include/uapi" -I"$repo/src" -I"$repo" \
	"$repo/plan/ws004/tests/usb-storage-no-media-test.c" \
	"$repo/src/kern/io.c" -o "$sanitized"
ASAN_OPTIONS=detect_leaks=1 "$sanitized"

cc -ffunction-sections -fdata-sections -Wl,--gc-sections -std=c11 -O0 -Wall -Wextra -Werror -fanalyzer \
	-I"$repo/include" -I"$repo/include/uapi" -I"$repo/src" -I"$repo" \
	"$repo/plan/ws004/tests/usb-storage-no-media-test.c" \
	"$repo/src/kern/io.c" -o "$analyzed"
"$analyzed"

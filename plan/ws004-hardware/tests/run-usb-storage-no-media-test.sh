#!/bin/sh
set -eu

repo=$(CDPATH= cd -- "$(dirname -- "$0")/../../.." && pwd)
temporary=${TMPDIR:-$repo/build/q047-tmp}/usb-storage-no-media
ordinary=$temporary/ordinary
sanitized=$temporary/sanitized
analyzed=$temporary/analyzed

mkdir -p "$temporary"

cc -std=c11 -O2 -Wall -Wextra -Werror \
	-I"$repo/include" -I"$repo/include/uapi" -I"$repo/src" -I"$repo" \
	"$repo/plan/ws004-hardware/tests/usb-storage-no-media-test.c" \
	-o "$ordinary"
"$ordinary"

cc -std=c11 -O1 -g -Wall -Wextra -Werror \
	-fsanitize=address,undefined -fno-omit-frame-pointer \
	-I"$repo/include" -I"$repo/include/uapi" -I"$repo/src" -I"$repo" \
	"$repo/plan/ws004-hardware/tests/usb-storage-no-media-test.c" \
	-o "$sanitized"
ASAN_OPTIONS=detect_leaks=1 "$sanitized"

cc -std=c11 -O0 -Wall -Wextra -Werror -fanalyzer \
	-I"$repo/include" -I"$repo/include/uapi" -I"$repo/src" -I"$repo" \
	"$repo/plan/ws004-hardware/tests/usb-storage-no-media-test.c" \
	-o "$analyzed"
"$analyzed"

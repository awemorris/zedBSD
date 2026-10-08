#!/bin/sh
# ws177-p017: builds the host's libbrowser.so (plan/ws074/tests/host-build.sh, into build/ws177-host by default) and a
# client of the public <browser.h> alone (host-focus-field.c), and runs it: browser_view_focus_field.
#   sh plan/ws177/tests/host-focus-field.sh [plain|asan]
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
set -eu
cd "$(dirname -- "$0")/../../.."
variant=${1:-plain}
base=${BROWSER_HOST_BUILD:-build/ws177-host}
flags='-std=gnu11 -O1 -g -Wall -Wextra -Werror'
if [ "$variant" = asan ]; then
	flags="$flags -fsanitize=address,undefined -fno-omit-frame-pointer -fno-sanitize-recover=undefined"
	ASAN_OPTIONS=${ASAN_OPTIONS:-detect_stack_use_after_return=0}
	export ASAN_OPTIONS
fi
BROWSER_HOST_BUILD="$base" sh plan/ws074/tests/host-build.sh "$variant"
${CC:-cc} $flags -Iuserland/desktop/include -o "$base/$variant/host-focus-field" plan/ws177/tests/host-focus-field.c \
	-L"$base/$variant" -Wl,-rpath,'$ORIGIN' -l:libbrowser.so -lvulkan -ldl
timeout 60 "$base/$variant/host-focus-field"

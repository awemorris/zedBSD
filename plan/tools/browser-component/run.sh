#!/bin/sh
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
# Builds the public-only dynamic client and runs its bounded loopback/Vulkan fixture.
#   sh plan/tools/browser-component/run.sh [plain|asan] [threads]
# threads runs the client's views on their own threads (BROWSER_THREADING_OWN, client.c --threads).
set -eu
cd "$(dirname -- "$0")/../../.."
variant=${1:-plain}
threads=
case "${2:-}" in
'') ;;
threads) threads=--threads ;;
*) echo 'usage: run.sh [plain|asan] [threads]' >&2; exit 2 ;;
esac
base=${BROWSER_HOST_BUILD:-build/ws107-host}
cc=${CC:-cc}
flags='-std=gnu11 -O1 -g -Wall -Wextra -Werror'
case "$variant" in
plain) ;;
asan)
    flags="$flags -fsanitize=address,undefined -fno-omit-frame-pointer -fno-sanitize-recover=undefined"
    ASAN_OPTIONS=${ASAN_OPTIONS:-detect_stack_use_after_return=0}
    export ASAN_OPTIONS
    ;;
*) echo 'usage: run.sh [plain|asan]' >&2; exit 2 ;;
esac
BROWSER_HOST_BUILD="$base" sh plan/ws074/tests/host-build.sh "$variant"
"$cc" $flags -Iuserland/desktop/include -o "$base/$variant/component-client" \
    -I. plan/tools/browser-component/client.c userland/tests/browser-threads/threads-posix.c -L"$base/$variant" -Wl,-rpath,'$ORIGIN' \
    -Wl,--export-dynamic-symbol=strdup,--export-dynamic-symbol=vkCreateFramebuffer \
    -l:libbrowser.so -lvulkan -ldl
python3 plan/tools/browser-component/run.py $threads "$base/$variant/component-client"

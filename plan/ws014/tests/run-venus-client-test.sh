#!/bin/sh
# Verify production shared Venus ownership and wire bounds with two finite peers.
set -eu
repo=$(CDPATH= cd -- "$(dirname -- "$0")/../../.." && pwd)
work=$(mktemp -d "${TMPDIR:-/tmp}/ws014-venus-client.XXXXXX")
trap 'rm -rf -- "$work"' EXIT HUP INT TERM

# Link the unchanged production logic to deterministic descriptor/transport peers.
cc -std=c89 -D_POSIX_C_SOURCE=200809L -O2 -Wall -Wextra -Werror \
    -Wdeclaration-after-statement -Wshadow -Wconversion \
    -Dopen=client_test_open -Dclose=client_test_close \
    -Dioctl=client_test_ioctl -Dnanosleep=client_test_nanosleep \
    -I"$repo/include" -c "$repo/userland/gpu/venus/client.c" \
    -o "$work/client.o"
cc -std=c89 -D_POSIX_C_SOURCE=200809L -O2 -Wall -Wextra -Werror \
    -Wdeclaration-after-statement -Wshadow -Wconversion \
    -I"$repo/include" "$repo/plan/ws014/tests/venus-client.c" \
    "$work/client.o" -o "$work/ordinary"
"$work/ordinary"

# Repeat only this ownership fixture under address and undefined-behavior checks.
cc -std=c89 -D_POSIX_C_SOURCE=200809L -O1 -g -Wall -Wextra -Werror \
    -Wdeclaration-after-statement -Wshadow -Wconversion \
    -fsanitize=address,undefined -fno-omit-frame-pointer \
    -Dopen=client_test_open -Dclose=client_test_close \
    -Dioctl=client_test_ioctl -Dnanosleep=client_test_nanosleep \
    -I"$repo/include" -c "$repo/userland/gpu/venus/client.c" \
    -o "$work/client-sanitized.o"
cc -std=c89 -D_POSIX_C_SOURCE=200809L -O1 -g -Wall -Wextra -Werror \
    -Wdeclaration-after-statement -Wshadow -Wconversion \
    -fsanitize=address,undefined -fno-omit-frame-pointer \
    -I"$repo/include" "$repo/plan/ws014/tests/venus-client.c" \
    "$work/client-sanitized.o" -o "$work/sanitized"
ASAN_OPTIONS=detect_leaks=1 UBSAN_OPTIONS=halt_on_error=1 "$work/sanitized"

# Existing guest headers use restrict; preserve that spelling for this C89 check.
cc -std=c89 -Drestrict=__restrict -Wall -Wextra -Werror \
    -Wdeclaration-after-statement -Wshadow -Wconversion -fsyntax-only \
    -I"$repo/libc/include" -I"$repo/include" \
    "$repo/userland/gpu/venus/client.c" "$repo/userland/gpu/venus/venus-frame.c"

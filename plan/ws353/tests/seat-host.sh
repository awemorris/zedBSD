#!/bin/sh
# Builds and runs the host test of the seat socket and the devices' thread's wait (seat-host-test.c): sessiond's
# seat.c and libkeiland-backend's backend.c, events-zedbsd.c and input-zedbsd.c on the host, under the sanitizers.
#   sh plan/ws353/tests/seat-host.sh [build-dir]
# The last line is "seat-host: PASS N checks" or "seat-host: FAIL ...".
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
set -eu
cd "$(dirname -- "$0")/../../.."
out=${1:-build/ws353-seat-host}
mkdir -p "$out"
${CC:-cc} -std=gnu89 -Wall -Wextra -Werror -D_GNU_SOURCE -I. -Iinclude -fsanitize=address,undefined -g -pthread \
    userland/desktop/libkeiland-backend/backend.c userland/desktop/libkeiland-backend/session/session-none.c \
    userland/desktop/libkeiland-backend/unsupported/seat-unsupported.c \
    userland/desktop/libkeiland-backend/unsupported/notify-unsupported.c \
    userland/desktop/libkeiland-backend-zedbsd/events-zedbsd.c userland/desktop/libkeiland-backend-zedbsd/input-zedbsd.c \
    userland/desktop/sessiond/seat.c plan/ws353/tests/seat-host-test.c -o "$out/seat-host"
exec timeout 60 "$out/seat-host"

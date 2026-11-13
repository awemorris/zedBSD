#!/bin/sh
# Builds and runs the host test of the compositor backend's system events (ws132-p003): libkeiland-backend's
# backend.c and the zedBSD events-zedbsd.c on the host, with a pipe for /dev/system, under the sanitizers.
#   sh plan/tools/system-events/run-host-events.sh [build-dir]
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
set -eu
cd "$(dirname -- "$0")/../../.."
out=${1:-build/ws132-host}
mkdir -p "$out"
${CC:-cc} -std=gnu89 -Wall -Wextra -Werror -D_GNU_SOURCE -I. -Iinclude -fsanitize=address,undefined -g \
    userland/desktop/libkeiland-backend/backend.c userland/desktop/libkeiland-backend/session/session-none.c \
    userland/desktop/libkeiland-backend/unsupported/seat-unsupported.c \
    userland/desktop/libkeiland-backend/unsupported/notify-unsupported.c \
    userland/desktop/libkeiland-backend-zedbsd/events-zedbsd.c userland/desktop/libkeiland-backend-zedbsd/input-zedbsd.c \
    plan/tools/system-events/host-events.c -o "$out/host-events"
exec "$out/host-events"

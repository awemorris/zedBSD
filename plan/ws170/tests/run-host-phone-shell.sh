#!/bin/sh
# ws170-p004: builds and runs the host test of the phone on the wire (host-phone-shell.c): the compositor's
# userland/desktop/wayland/phone-shell.c with capture clients and libkeiland's ring of phone events
# (userland/desktop/libkeiland/system/system-view.c), under ASan and UBSan.
#   sh plan/ws170/tests/run-host-phone-shell.sh [OUTPUT]   (default build/ws170/host-phone-shell)
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
set -eu
cd "$(dirname -- "$0")/../../.."
out=${1:-build/ws170/host-phone-shell}
mkdir -p "$(dirname -- "$out")/include/keiland"
cp userland/desktop/include/keiland/keiland.h "$(dirname -- "$out")/include/keiland/keiland.h"
${CC:-cc} -std=gnu99 -D_GNU_SOURCE -O1 -g -Wall -Wextra -Werror \
	-fsanitize=address,undefined -fno-sanitize-recover=all -fno-omit-frame-pointer \
	-I. -I"$(dirname -- "$out")/include" \
	plan/ws170/tests/host-phone-shell.c userland/desktop/wayland/phone-shell.c \
	userland/desktop/libkeiland-backend/unsupported/phone-unsupported.c \
	userland/desktop/libkeiland/system/system-view.c -o "$out"
timeout 60 "$out"

#!/bin/sh
# q893 (ws177-p019 (b)): builds the keyboard's layouts (userland/desktop/wayland/keyboard-layout.c) with the host's
# C compiler and host-osk-field.c against them, and runs it.
#   sh plan/ws177/tests/host-osk-field.sh
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
set -u
cd "$(dirname -- "$0")/../../.."
out=build/ws177-host
mkdir -p "$out"
${CC:-cc} -O2 -g -Wall -Wextra -Werror -I. -Iuserland/desktop/wayland -DKEILAND_DATADIR='"/nonexistent"' -o "$out/host-osk-field" \
    plan/ws177/tests/host-osk-field.c userland/desktop/wayland/keyboard-layout.c userland/desktop/wayland/keyboard-hand.c \
    userland/desktop/wayland/hand-cloud.c -lm || exit 1
"$out/host-osk-field"

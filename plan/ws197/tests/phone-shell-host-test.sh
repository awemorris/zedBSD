#!/bin/sh
# The host test of the compositor's phone messages and libkeiland's view of them (ws197-p004a, plan/ws197/phase004/phase.md
# section 12): builds plan/ws197/tests/phone-shell-host-test.c with userland/desktop/wayland/phone-shell.c (a fake
# kl_backend_phone and capture clients) and userland/desktop/libkeiland/system/system-view.c, with the host's compiler under
# ASan and UBSan, and runs it.  The last line is "phone-shell-host-test: PASS" or "... FAIL".
# usage: plan/ws197/tests/phone-shell-host-test.sh   (from the repository's top; OUT= to choose the build folder)
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
set -eu
OUT=${OUT:-build/ws197-phone-shell-host}
mkdir -p "$OUT"
flags="-std=gnu11 -D_GNU_SOURCE -Wall -Wextra -Werror -Wno-format-truncation -O1 -g -fsanitize=address,undefined -fno-sanitize-recover=all -I. -Iuserland/desktop/include"
cc $flags -o "$OUT/phone-shell-host-test" plan/ws197/tests/phone-shell-host-test.c userland/desktop/wayland/phone-shell.c \
	userland/desktop/libkeiland/system/system-view.c
timeout 60 "$OUT/phone-shell-host-test"

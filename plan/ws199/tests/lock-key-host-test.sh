#!/bin/sh
# ws199-p003 (i05): the security key's mode of the login and lock screen (lock-key.c) and the PIN's keypad
# (lock-keypad.c), built with the host's compiler under ASan and UBSan.
# usage: plan/ws199/tests/lock-key-host-test.sh   (from the repository's top; OUT= to choose the build folder)
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
set -eu
OUT=${OUT:-build/ws199-lock-key-host}
mkdir -p "$OUT"
cc -std=c11 -D_DEFAULT_SOURCE -Wall -Wextra -Werror -O1 -g -fsanitize=address,undefined -fno-sanitize-recover=all -I. \
	-o "$OUT/lock-key-host-test" plan/ws199/tests/lock-key-host-test.c userland/desktop/wayland/lock-key.c \
	userland/desktop/wayland/lock-keypad.c
timeout 60 "$OUT/lock-key-host-test"

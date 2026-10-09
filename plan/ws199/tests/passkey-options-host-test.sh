#!/bin/sh
# ws199-p002 (d), p003: /sbin/passkey's account options (read, written back, the defaults) and the keys' operations'
# requests, under ASan and UBSan.
# usage: plan/ws199/tests/passkey-options-host-test.sh   (from the repository's top; OUT= to choose the build folder)
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
set -eu
OUT=${OUT:-build/ws199-passkey-options-host}
mkdir -p "$OUT"
cc -std=c11 -D_DEFAULT_SOURCE -Wall -Wextra -Werror -O1 -g -fsanitize=address,undefined -fno-sanitize-recover=all -I. \
	-o "$OUT/passkey-options-host-test" plan/ws199/tests/passkey-options-host-test.c userland/base/passkey/request.c \
	userland/base/passkey/record.c
timeout 60 "$OUT/passkey-options-host-test"

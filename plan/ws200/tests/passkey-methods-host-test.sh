#!/bin/sh
# ws200-p001: /sbin/passkey's sign-in methods (record.c's methods, request.c's set-methods), under ASan and UBSan.
# usage: plan/ws200/tests/passkey-methods-host-test.sh   (from the repository's top; OUT= to choose the build folder)
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
set -eu
OUT=${OUT:-build/ws200-passkey-methods-host}
mkdir -p "$OUT"
cc -std=c11 -D_DEFAULT_SOURCE -Wall -Wextra -Werror -O1 -g -fsanitize=address,undefined -fno-sanitize-recover=all -I. \
	-o "$OUT/passkey-methods-host-test" plan/ws200/tests/passkey-methods-host-test.c userland/base/passkey/request.c \
	userland/base/passkey/record.c
timeout 60 "$OUT/passkey-methods-host-test"

#!/bin/sh
# The host test of Phone's store of the paired phone's messages (ws197-p004b, plan/ws197/phase004/phase.md sections 2 and 6):
# builds plan/ws197/tests/phone-store-host-test.c with userland/desktop/phone/store.c userland/desktop/phone/phonebook.c, with the host's compiler under ASan and
# UBSan, and runs it in a new folder of the build.  The last line is "phone-store-host-test: PASS" or "... FAIL".
# usage: plan/ws197/tests/phone-store-host-test.sh   (from the repository's top; OUT= to choose the build folder)
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
set -eu
OUT=${OUT:-build/ws197-phone-store-host}
mkdir -p "$OUT"
run=$(mktemp -d "$OUT/run.XXXXXX")
flags="-std=gnu11 -D_GNU_SOURCE -Wall -Wextra -Werror -Wno-format-truncation -O1 -g -fsanitize=address,undefined -fno-sanitize-recover=all -I. -Iuserland/desktop/include"
cc $flags -o "$OUT/phone-store-host-test" plan/ws197/tests/phone-store-host-test.c userland/desktop/phone/store.c userland/desktop/phone/phonebook.c
timeout 60 "$OUT/phone-store-host-test" "$run/Phone"

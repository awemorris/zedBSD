#!/bin/sh
# Runs PBAP persistence against the production store with ASan, UBSan and LSan.
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
set -eu
OUT=${OUT:-build/ws197-pbap-store-host}
mkdir -p "$OUT"
run=$(mktemp -d "$OUT/run.XXXXXX")
cc -std=gnu11 -D_GNU_SOURCE -Wall -Wextra -Werror -Wdeclaration-after-statement -Wno-format-truncation -O1 -g -fsanitize=address,undefined -fno-sanitize-recover=all -I. -Iuserland/desktop/include -o "$OUT/phone-pbap-store-host-test" plan/ws197/tests/phone-pbap-store-host-test.c userland/desktop/phone/store.c userland/desktop/phone/phonebook.c
timeout 120 "$OUT/phone-pbap-store-host-test" "$run"

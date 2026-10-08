#!/bin/sh
# BUG-265's host test: builds userland/desktop/wayland/title-tap.c with plan/bugs/BUG-265/title-tap-host.c under ASan and
# UBSan and runs it.  usage: plan/bugs/BUG-265/title-tap-host.sh   (from the repository's top; OUT= to choose the build folder)
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
set -eu
OUT=${OUT:-build/bug265-host}
mkdir -p "$OUT"
cc -std=gnu11 -Wall -Wextra -Werror -O1 -g -fsanitize=address,undefined -fno-sanitize-recover=all -I. \
	-o "$OUT/title-tap-host" plan/bugs/BUG-265/title-tap-host.c userland/desktop/wayland/title-tap.c
timeout 60 "$OUT/title-tap-host"

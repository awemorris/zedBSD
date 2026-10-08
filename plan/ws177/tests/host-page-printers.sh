#!/bin/sh
# ws177-p025: builds Settings' Printers page (page-printers.c) with host-page-printers.c (stand-ins of the widgets and
# the desktop's printers) on the host, under ASan and UBSan, and runs it, in a new folder under build/tmp (nothing is
# removed here; Q1's plan/tools/q1-clean.sh removes old runs).
#   sh plan/ws177/tests/host-page-printers.sh
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
set -eu
repo=$(CDPATH= cd -- "$(dirname -- "$0")/../../.." && pwd)
cd "$repo"
. plan/tools/fresh-out.sh
fresh_out "$repo/build/tmp/ws177-page-printers"
work=$fresh_dir
mkdir -p "$work/include/keiland"
cp userland/desktop/include/keiland/keiland.h "$work/include/keiland/keiland.h"
cc -std=gnu99 -D_GNU_SOURCE -O1 -g -Wall -Wextra -Werror -pthread -fsanitize=address,undefined -fno-sanitize-recover=all \
	-fno-omit-frame-pointer -I. -I"$work/include" -Iuserland/desktop/include -Iuserland/desktop/settings \
	plan/ws177/tests/host-page-printers.c userland/desktop/settings/page-printers.c -o "$work/host-page-printers"
timeout 60 "$work/host-page-printers"

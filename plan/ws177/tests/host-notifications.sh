#!/bin/sh
# ws177-p026: builds and runs, on the host under ASan and UBSan, the compositor's gate on notifications (notify-shell.c
# with notify.c, host-notify-allow.c) and Settings' Notifications page (page-notifications.c, host-page-notifications.c),
# in a new folder under build/tmp (nothing is removed here; Q1's plan/tools/q1-clean.sh removes old runs).
#   sh plan/ws177/tests/host-notifications.sh
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
set -eu
repo=$(CDPATH= cd -- "$(dirname -- "$0")/../../.." && pwd)
cd "$repo"
. plan/tools/fresh-out.sh
fresh_out "$repo/build/tmp/ws177-notifications"
work=$fresh_dir
mkdir -p "$work/include/keiland"
cp userland/desktop/include/keiland/keiland.h "$work/include/keiland/keiland.h"
flags="-std=gnu99 -D_GNU_SOURCE -O1 -g -Wall -Wextra -Werror -pthread -fsanitize=address,undefined -fno-sanitize-recover=all -fno-omit-frame-pointer"
cc $flags -I. -I"$work/include" plan/ws177/tests/host-notify-allow.c userland/desktop/wayland/notify-shell.c \
	userland/desktop/wayland/notify.c -o "$work/host-notify-allow"
ASAN_OPTIONS=detect_leaks=0 timeout 60 "$work/host-notify-allow"
cc $flags -I. -I"$work/include" -Iuserland/desktop/include -Iuserland/desktop/settings plan/ws177/tests/host-page-notifications.c \
	userland/desktop/settings/page-notifications.c -o "$work/host-page-notifications"
timeout 60 "$work/host-page-notifications"

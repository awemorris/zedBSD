#!/bin/sh
# ws131-p010: builds the host test of Keiland's system extension -- libkeiland's view and kl_system_* client
# (userland/desktop/libkeiland/system/) with the host's libwayland-client, against the compositor's
# userland/desktop/wayland/system.c with fakes of the network, the sound and the power -- under ASan and UBSan,
# and runs it.
#   sh plan/ws131/tests/host-system.sh [OUTPUT]   (default build/ws131-host/host-system)
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
set -eu
cd "$(dirname -- "$0")/../../.."
out=${1:-build/ws131-host/host-system}
mkdir -p "$(dirname -- "$out")/include/keiland"
# Only keiland/keiland.h from the tree: the tree's wayland-client.h is zedBSD's, the host's is the one to link with.
cp userland/desktop/include/keiland/keiland.h "$(dirname -- "$out")/include/keiland/keiland.h"
${CC:-cc} -std=gnu99 -D_GNU_SOURCE -O1 -g -Wall -Wextra -Werror -pthread \
	-fsanitize=address,undefined -fno-sanitize-recover=all -fno-omit-frame-pointer \
	-I. -I"$(dirname -- "$out")/include" \
	plan/ws131/tests/host-system.c userland/desktop/wayland/system.c userland/desktop/wayland/sysmon.c \
	userland/desktop/wayland/notify.c userland/desktop/wayland/notify-shell.c userland/desktop/wayland/mail-shell.c userland/desktop/wayland/phone-shell.c userland/desktop/wayland/printers-shell.c \
	userland/desktop/libkeiland-backend/print/print.c \
	userland/desktop/wayland/machine-shell.c userland/desktop/wayland/machine-wait.c userland/desktop/wayland/language-file.c \
	userland/desktop/libkeiland-backend/machine/machine.c userland/desktop/libkeiland-backend/machine/filesystems.c \
	userland/desktop/libkeiland-backend/machine/users.c userland/desktop/libkeiland-backend-linux/users-linux.c \
	userland/desktop/libkeiland-backend/machine/mounts.c userland/desktop/libkeiland-backend/machine/mounts-mntent.c \
	userland/desktop/libkeiland/system/system.c userland/desktop/libkeiland/system/system-view.c \
	userland/desktop/libkeiland/system/system-protocol.c \
	$(pkg-config --cflags --libs wayland-client) -o "$out"
timeout 120 "$out"

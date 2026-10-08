#!/bin/sh
# ws188-p002: builds and runs the host test of the computer's reading -- libkeiland-backend's machine area, the
# compositor's queries waiting (machine-wait.c) and libkeiland's view of the answers (system-view.c) -- under ASan and
# UBSan.  The end-to-end path (libkeiland -> compositor -> backend) is plan/ws131/tests/host-system.sh's machine step.
#   sh plan/tools/keiland-machine/host-machine.sh [OUTPUT]   (default build/keiland-machine-host; a fresh run each time)
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
set -eu
cd "$(dirname -- "$0")/../../.."
. plan/tools/fresh-out.sh
out=${1:-build/keiland-machine-host}
fresh_out "$out"
mkdir -p "$out/include/keiland"
# Only keiland/keiland.h from the tree (the host's libc and headers otherwise).
cp userland/desktop/include/keiland/keiland.h "$out/include/keiland/keiland.h"
${CC:-cc} -std=gnu99 -D_GNU_SOURCE -O1 -g -Wall -Wextra -Werror \
	-fsanitize=address,undefined -fno-sanitize-recover=all -fno-omit-frame-pointer \
	-I. -I"$out/include" \
	plan/tools/keiland-machine/host-machine.c \
	userland/desktop/libkeiland-backend/machine/machine.c userland/desktop/libkeiland-backend/machine/filesystems.c \
	userland/desktop/libkeiland-backend/machine/users.c userland/desktop/wayland/machine-wait.c \
	userland/desktop/libkeiland-backend/machine/mounts.c userland/desktop/libkeiland-backend/machine/mounts-mntent.c \
	userland/desktop/files/mounts.c \
	userland/desktop/libkeiland/system/system-view.c \
	-o "$out/host-machine"
timeout 60 "$out/host-machine" "$out"

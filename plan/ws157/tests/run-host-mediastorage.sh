#!/bin/sh
# zedBSD; Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
# Source-based media regression suite; requires the already built private Linux desktop libraries.
# Usage: sh plan/ws157/tests/run-host-mediastorage.sh HOST_BUILD LINUX_BUILD
set -eu
cd "$(dirname -- "$0")/../../.."
media_host=$(realpath "${1:-build/ws197-media-library-host}")
media_linux=$(realpath "${2:-build/ws197-media-library-linux}")
mkdir -p "$media_host/bin" "$media_host/inc/keiland"
cp include/libc/sha2.h "$media_host/inc/sha2.h"
cp userland/desktop/include/keiland/keiland.h "$media_host/inc/keiland/keiland.h"
${CC:-cc} -std=gnu99 -D_GNU_SOURCE -O1 -g -w -I"$media_host/inc" \
	-c src/libc/openbsd-sha2.c -o "$media_host/sha2.o"
${CC:-cc} -std=gnu99 -D_GNU_SOURCE -O1 -g -Wall -Wextra -Werror \
	-fsanitize=address,undefined -fno-sanitize-recover=all -fno-omit-frame-pointer \
	-I. -Iuserland/desktop/include -I"$media_host/inc" \
	userland/desktop/mediastorage/main.c userland/desktop/mediastorage/notify.c \
	userland/desktop/mediastorage/database.c userland/desktop/mediastorage/json.c \
	userland/desktop/mediastorage/dimensions.c userland/desktop/photos/cache-folders.c \
	userland/desktop/photos/import.c userland/desktop/photos/exif.c \
	userland/desktop/photos/library.c userland/desktop/photos/snapshot.c "$media_host/sha2.o" \
	-L"$media_linux/lib" -Wl,-rpath-link,"$media_linux/lib" -lkeiland -lwayland-client \
	-o "$media_host/bin/mediastorage"
${CC:-cc} -std=gnu99 -D_GNU_SOURCE -O1 -g -Wall -Wextra -Werror \
	-fsanitize=address,undefined -fno-sanitize-recover=all -fno-omit-frame-pointer \
	-I. "-DKEILAND_BINDIR=\"$media_host/bin\"" \
	plan/ws157/tests/host-media-backend.c userland/desktop/libkeiland-backend/media/media.c \
	-o "$media_host/host-media-backend"
${CC:-cc} -std=gnu99 -D_GNU_SOURCE -O1 -g -Wall -Wextra -Werror \
	-fsanitize=address,undefined -fno-sanitize-recover=all -fno-omit-frame-pointer \
	-I. -I"$media_host/inc" "-DKEILAND_BINDIR=\"$media_host/bin\"" \
	plan/ws157/tests/host-media-wire.c userland/desktop/wayland/media-library.c \
	userland/desktop/libkeiland-backend/media/media.c \
	userland/desktop/libkeiland/system/system-protocol.c \
	-L"$media_linux/lib" -Wl,-rpath-link,"$media_linux/lib" \
	-lkeiland -lwayland-server -lwayland-client -lpthread -o "$media_host/host-media-wire"
${CC:-cc} -std=gnu99 -D_GNU_SOURCE -O1 -g -Wall -Wextra -Werror \
	-fsanitize=address,undefined -fno-sanitize-recover=all -fno-omit-frame-pointer \
	-I. -Iuserland/desktop/include plan/ws197/tests/host-media-select.c \
	userland/desktop/phone/media.c userland/desktop/libkeiland/ui/chooser-model.c \
	-L"$media_linux/lib" -Wl,-rpath-link,"$media_linux/lib" -lkeiland -lwayland-client \
	-o "$media_host/host-media-select"
export LD_LIBRARY_PATH="$media_linux/lib${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
python3 plan/ws157/tests/host-media-library.py "$media_host/bin/mediastorage"
media_wire=$(mktemp -d /tmp/media-wire-test-XXXXXX)
timeout 30 "$media_host/host-media-wire" "$media_wire"
media_backend=$(mktemp -d /tmp/media-backend-test-XXXXXX)
timeout 30 "$media_host/host-media-backend" "$media_backend"
media_selection=$(mktemp -d /tmp/media-selection-test-XXXXXX)
timeout 30 "$media_host/host-media-select" "$media_selection"

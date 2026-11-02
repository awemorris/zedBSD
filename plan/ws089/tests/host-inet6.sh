#!/bin/sh
# BUG-413: builds and runs the host test of a network link's IPv6 in libkeiland's system extension (host-inet6.c):
# the protocol's tables (link_inet6 and link_router6 since version 35) and the view with an older compositor (no
# IPv6) and a newer one.  libkeiland's system-view.c and system-protocol.c with the host's libwayland-client
# headers, under ASan and UBSan.
#   sh plan/ws089/tests/host-inet6.sh [OUTPUT]   (default build/ws089-host/host-inet6)
# PASS: the last line host-inet6: PASS.
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
set -eu
cd "$(dirname -- "$0")/../../.."
out=${1:-build/ws089-host/host-inet6}
mkdir -p "$(dirname -- "$out")/inet6-include/keiland"
# Only keiland/keiland*.h from the tree: the tree's wayland-client.h is zedBSD's, the host's is the one to use.
cp userland/desktop/include/keiland/keiland*.h "$(dirname -- "$out")/inet6-include/keiland/"
${CC:-cc} -std=gnu99 -D_GNU_SOURCE -O1 -g -Wall -Wextra -Werror \
	-fsanitize=address,undefined -fno-sanitize-recover=all -fno-omit-frame-pointer \
	-I. -I"$(dirname -- "$out")/inet6-include" \
	plan/ws089/tests/host-inet6.c userland/desktop/libkeiland/system/system-view.c \
	userland/desktop/libkeiland/system/system-protocol.c \
	$(pkg-config --cflags --libs wayland-client) -o "$out"
timeout 60 "$out"

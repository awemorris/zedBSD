#!/bin/sh
# Builds and runs configure-test.c (libkeiland's deferred configures) against a Linux build of libkeiland,
# with configure-server.c as the compositor.  Last line: PASS ... or FAIL ....
#
#   sh plan/ws345/tests/configure-test.sh LINUX_BUILD     # e.g. build/ws345-p013-linux
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
set -u
cd "$(dirname -- "$0")/../../.."
build=$1
lib=$build/lib
out=build/tmp/ws345-configure-test
mkdir -p "$out/gen"
xml=/usr/share/wayland-protocols/stable/xdg-shell/xdg-shell.xml
wayland-scanner server-header "$xml" "$out/gen/xdg-shell-server.h" || exit 1
wayland-scanner private-code "$xml" "$out/gen/xdg-shell-protocol.c" || exit 1
case $build in *tsan*) sanitize=-fsanitize=thread ;; *) sanitize= ;; esac
# The server's library comes first: its core interfaces (wl_shm 2) are a superset of the client library's, which both use.
cc $sanitize -std=gnu17 -Wall -Wextra -Werror -O1 -g -Iuserland/desktop/include -I"$build/include" -I"$out/gen" \
	-Iplan/ws345/tests plan/ws345/tests/configure-test.c plan/ws345/tests/configure-server.c "$out/gen/xdg-shell-protocol.c" \
	-o "$out/configure-test" -lwayland-server -L"$lib" -lkeiland -l:libwayland-client.so -pthread \
	-Wl,-rpath,"$PWD/$lib" || exit 1
dir=$(mktemp -d "$PWD/$out/run.XXXXXX")
chmod 700 "$dir"
LD_LIBRARY_PATH="$PWD/$lib" XDG_RUNTIME_DIR=$dir timeout 120 "$out/configure-test"

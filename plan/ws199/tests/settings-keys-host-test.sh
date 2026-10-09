#!/bin/sh
# ws199-p004: the Security Keys page's wizards (page-users-keys.c, its static steps reached by including it) on the
# host, linked with Settings' host build (plan/ws089/tests/host-build.sh, built first) without its own copy of the page.
# usage: plan/ws199/tests/settings-keys-host-test.sh   (from the repository's top)
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
set -eu
cd "$(dirname -- "$0")/../../.."
sh plan/ws089/tests/host-build.sh > /dev/null
out=build/ws089-host
objects=$(grep -v -e '/settings-page-users-keys\.o$' "$out/objects.list")
${CC:-cc} -O1 -g -std=gnu89 -Wall -Wextra -Werror -Wno-unused-parameter -Wno-unused-function -D_GNU_SOURCE \
	-I"$out/include" -Iuserland/desktop/settings -I. -o "$out/settings-keys-host-test" \
	plan/ws199/tests/settings-keys-host-test.c "$out/obj/host-network.o" $objects -lm -pthread
HOME="$out" timeout 60 "$out/settings-keys-host-test" > "$out/settings-keys-host-test.txt"
tail -n 1 "$out/settings-keys-host-test.txt"
grep -q 'settings-keys-host-test: PASS' "$out/settings-keys-host-test.txt"

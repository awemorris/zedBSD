#!/bin/sh
# ws192-p001: builds and runs the status panel's host test (status-panel-host-test.c) against the compositor's
# status-panel.c and plane.c with the host's compiler, the Linux build's flags.  Output: build/ws192-host (a fresh
# directory each run).  The last line is status-panel-host-test: PASS or FAIL.
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
set -u
cd "$(dirname "$0")/../../.." || exit 2
. plan/tools/fresh-out.sh
fresh_out build/ws192-host
out=build/ws192-host
cc -O1 -g -std=gnu17 -Wall -Wextra -Werror -Wno-format-truncation -D_GNU_SOURCE -DKEILAND_DATADIR='"/usr/share/keiland"' \
	-I. -Iuserland/desktop/include -Ibuild/keiland-linux/include \
	plan/ws192/tests/status-panel-host-test.c userland/desktop/wayland/status-panel.c userland/desktop/wayland/plane.c \
	-o "$out/status-panel-host-test" || { echo "status-panel-host-test: FAIL (build)"; exit 1; }
"$out/status-panel-host-test" "$out/panel.log"

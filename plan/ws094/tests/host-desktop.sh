#!/bin/sh
# ws094-p003: builds files' host objects (plan/tools/files/host-build.sh) and host-desktop.c against them, and runs it.
#   sh plan/ws094/tests/host-desktop.sh
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
set -u
cd "$(dirname -- "$0")/../../.."
# The objects this build makes (host-build.sh compiles every one again): older ones with names it no longer uses are
# left out (BUG-221, 2026-10-08: they defined the same symbols twice).
mkdir -p build/tmp
stamp=$(mktemp "$(pwd)/build/tmp/host-desktop-stamp.XXXXXX")
sh plan/tools/files/host-build.sh >/dev/null || exit 1
out=build/ws071-host
objects=$(find $out/obj -name '*.o' -newer "$stamp" ! -name 'host-*' | sort)
${CC:-cc} -O2 -g -Wall -Wextra -Werror -Wno-unused-parameter -D_GNU_SOURCE -I$out/include -Iuserland/desktop/files -I. \
    -o $out/host-desktop plan/ws094/tests/host-desktop.c $objects -lm || exit 1
# The work directory stays in build/tmp (2026-10-06 user: deleting is Q1's step; plan/tools/q1-clean.sh removes it).
temporary=$(mktemp -d "$(pwd)/build/tmp/host-desktop.XXXXXX")
$out/host-desktop "$temporary"
status=$?
exit $status

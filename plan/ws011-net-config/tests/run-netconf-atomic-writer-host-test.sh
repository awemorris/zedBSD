#!/bin/sh
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
# Host call-order/fault contract only: target overlay/UFS requires its own gate.
set -eu

repository_root=$(CDPATH= cd -- "$(dirname -- "$0")/../../.." && pwd)
cd "$repository_root"
mkdir -p plan/ws011-net-config/temp
test_root=$(mktemp -d plan/ws011-net-config/temp/atomic-writer.XXXXXX)
mkdir "$test_root/fixture"

sanitize_flags=
if [ "${NCOM_SANITIZE:-0}" = 1 ]; then
  sanitize_flags='-fsanitize=address,undefined -fno-omit-frame-pointer'
fi

# Flags are deliberately split into compiler arguments.
# shellcheck disable=SC2086
timeout --kill-after=5s 60s "${CC:-cc}" -std=c11 \
  -D_POSIX_C_SOURCE=200809L -D_DEFAULT_SOURCE -I. \
  -Wall -Wextra -Werror -O1 -g $sanitize_flags \
  plan/ws011-net-config/tests/netconf-atomic-writer-host-test.c \
  userland/base/net/netconf.c \
  -Wl,--wrap=open -Wl,--wrap=fdopen -Wl,--wrap=fflush \
  -Wl,--wrap=fsync -Wl,--wrap=fclose -Wl,--wrap=close \
  -Wl,--wrap=rename -Wl,--wrap=unlink \
  -o "$test_root/netconf-atomic-writer-host-test"

printf 'Host diagnostic artifacts: %s/%s\n' "$repository_root" "$test_root"
timeout --kill-after=5s 20s "$test_root/netconf-atomic-writer-host-test" \
  plan/ws011-net-config/tests/confirmed-commit-qemu-net.conf \
  "$test_root/fixture"

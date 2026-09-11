#!/bin/sh
set -eu

test_root=${TMPDIR:-/tmp}/ws001-link-unlink.$$
link_bin=$test_root/link
unlink_bin=$test_root/unlink
trap 'rm -rf "$test_root"' EXIT HUP INT TERM
mkdir -p "$test_root/work"

cc -std=c11 -D_POSIX_C_SOURCE=200809L -I. -Wall -Wextra -Werror \
  userland/base/link/main.c userland/base/common/command.c -o "$link_bin"
cc -std=c11 -D_POSIX_C_SOURCE=200809L -I. -Wall -Wextra -Werror \
  userland/base/unlink/main.c userland/base/common/command.c -o "$unlink_bin"

printf 'payload\n' >"$test_root/work/source"
"$link_bin" "$test_root/work/source" "$test_root/work/target"
test "$(stat -c %i "$test_root/work/source")" = \
  "$(stat -c %i "$test_root/work/target")"
"$unlink_bin" "$test_root/work/target"
test ! -e "$test_root/work/target"

"$link_bin" -- "$test_root/work/source" "$test_root/work/-target"
"$unlink_bin" -- "$test_root/work/-target"

if "$link_bin" "$test_root/work/source" "$test_root/work/source" \
    >/dev/null 2>&1; then
  echo 'link replaced an existing destination' >&2
  exit 1
fi
if "$link_bin" "$test_root/work/missing" "$test_root/work/new" \
    >/dev/null 2>&1; then
  echo 'link accepted a missing source' >&2
  exit 1
fi
if "$unlink_bin" "$test_root/work" >/dev/null 2>&1; then
  echo 'unlink removed a directory' >&2
  exit 1
fi
if "$link_bin" >/dev/null 2>&1 || "$link_bin" a b c >/dev/null 2>&1; then
  echo 'link accepted an invalid operand count' >&2
  exit 1
fi
if "$unlink_bin" >/dev/null 2>&1 ||
    "$unlink_bin" a b >/dev/null 2>&1; then
  echo 'unlink accepted an invalid operand count' >&2
  exit 1
fi

echo 'WS001 link/unlink: PASS'

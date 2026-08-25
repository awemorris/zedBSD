#!/bin/sh
set -eu

source_file=${1:-userland/base/basename/main.c}
binary=${TMPDIR:-/tmp}/ws001-basename-test
actual=${TMPDIR:-/tmp}/ws001-basename-actual
expected=${TMPDIR:-/tmp}/ws001-basename-expected

cc -std=c11 -D_POSIX_C_SOURCE=200809L -Wall -Wextra -Werror \
  "$source_file" -o "$binary"

check() {
  expected_text=$1
  shift
  printf '%s\n' "$expected_text" >"$expected"
  "$binary" "$@" >"$actual"
  cmp "$expected" "$actual"
}

check foo foo
check foo /usr/bin/foo
check foo /usr/bin/foo////
check / /
check / //
check / '/////'
check '' ''
check foo foo foo
check foo foo ''
check foo foobar bar
check foobar foobar foobar
check fo foobar obar
dash_value=-dash
check "$dash_value" -- "$dash_value"

if "$binary" >/dev/null 2>&1; then
  echo "basename accepted a missing operand" >&2
  exit 1
fi
if "$binary" a b c >/dev/null 2>&1; then
  echo "basename accepted too many operands" >&2
  exit 1
fi
if [ -e /dev/full ] && "$binary" value >/dev/full 2>/dev/null; then
  echo "basename ignored a stdout failure" >&2
  exit 1
fi

echo "WS001 basename: PASS"

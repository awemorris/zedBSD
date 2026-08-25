#!/bin/sh
set -eu

source_file=${1:-userland/base/dirname/main.c}
test_root=${TMPDIR:-/tmp}/ws001-dirname-test.$$
binary=$test_root/dirname
actual=$test_root/actual
expected=$test_root/expected

trap 'rm -rf "$test_root"' EXIT HUP INT TERM
mkdir -p "$test_root"
cc -std=c11 -D_POSIX_C_SOURCE=200809L -Wall -Wextra -Werror \
  "$source_file" -o "$binary"

check() {
  expected_text=$1
  shift
  printf '%s\n' "$expected_text" >"$expected"
  "$binary" "$@" >"$actual"
  cmp "$expected" "$actual"
}

check . foo
check . ''
check / /
check / //
check / '/////'
check / /foo
check / /foo/
check /usr /usr/bin
check /usr /usr//bin///
check a a/b
check a/b a/b/c
check . -- -dash

long_component=$(awk 'BEGIN { for (i = 0; i < 4096; i++) printf "x" }')
check "$long_component" "$long_component/value"

if "$binary" >/dev/null 2>&1; then
  echo "dirname accepted a missing operand" >&2
  exit 1
fi
if "$binary" a b >/dev/null 2>&1; then
  echo "dirname accepted too many operands" >&2
  exit 1
fi
if [ -e /dev/full ] && "$binary" value >/dev/full 2>/dev/null; then
  echo "dirname ignored a stdout failure" >&2
  exit 1
fi

echo "WS001 dirname: PASS"

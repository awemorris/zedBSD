#!/bin/sh
set -eu

test_root=${TMPDIR:-/tmp}/ws011-net-console.$$
binary=$test_root/net
output=$test_root/output
errors=$test_root/errors

trap 'rm -rf "$test_root"' EXIT HUP INT TERM
mkdir -p "$test_root"
cc -std=c11 -D_POSIX_C_SOURCE=200809L -D_DEFAULT_SOURCE -I. \
  -Iuserland/base/libedit -Wall -Wextra -Werror \
  userland/base/net/main.c userland/base/net/netconf.c \
  userland/base/net/wifi-conf.c userland/base/net/wifi-store.c \
  userland/base/service/service-config.c userland/base/libedit/readline.c \
  -o "$binary"

"$binary" help >"$output"
grep -q '^net commands:' "$output"
grep -q 'net dhcp interface \[--timeout=seconds\]' "$output"

printf '%s\n' \
  help configure 'interface ne0' 'dhcp timeout 12' exit \
  'show candidate' discard end exit |
  "$binary" >"$output" 2>"$errors"
grep -q 'net(config-if:ne0)>' "$output"
grep -q 'dhcp-timeout: 12' "$output"
grep -q 'Operational commands:' "$output"

printf '%s\n' \
  configure 'interface ne0' 'static ipv4 invalid prefix-length 24' exit \
  'show candidate' discard end exit |
  "$binary" >"$output" 2>"$errors"
grep -q 'invalid interface command' "$errors"
if grep -q 'address: invalid' "$output"; then
  echo 'invalid input mutated the candidate' >&2
  exit 1
fi

set +e
"$binary" dhcp ne0 >/dev/null 2>"$errors"
status=$?
set -e
if [ "$status" -eq 2 ]; then
  echo 'net dhcp ne0 was rejected as usage' >&2
  exit 1
fi

printf '%s\n' configure 'interface ne0' exit end exit n exit y |
  "$binary" >"$output" 2>"$errors"
grep -q 'Discard unsaved changes?' "$output"

echo 'WS011 net console: PASS'

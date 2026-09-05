#!/bin/sh
set -eu

test_root=${TMPDIR:-/tmp}/ws011-net-boot.$$
socket_path=$test_root/networkd.sock
binary=$test_root/net
server=$test_root/networkd-fake
server_pid=

cleanup()
{
  if [ -n "$server_pid" ]; then
    kill "$server_pid" 2>/dev/null || true
    wait "$server_pid" 2>/dev/null || true
  fi
  rm -rf "$test_root"
}
trap cleanup EXIT HUP INT TERM
mkdir -p "$test_root"

cc -std=c11 -D_POSIX_C_SOURCE=200809L -D_DEFAULT_SOURCE -I. \
  -Iuserland/base/libedit -Wall -Wextra -Werror \
  -DNETWORKD_SOCKET="\"$socket_path\"" \
  userland/base/net/main.c userland/base/net/netconf.c \
  userland/base/net/protocol.c \
  userland/base/net/wifi-conf.c userland/base/net/wifi-store.c \
  userland/base/libedit/readline.c -o "$binary"
cc -std=c11 -D_POSIX_C_SOURCE=200809L -Wall -Wextra -Werror \
  -I. plan/ws011-net-config/tests/networkd-fake.c \
  userland/base/net/protocol.c -o "$server"

run_case()
{
  configuration=$1
  shift
  rm -f "$socket_path"
  "$server" "$socket_path" "$@" &
  server_pid=$!
  attempts=0
  while [ ! -S "$socket_path" ]; do
    attempts=$((attempts + 1))
    if [ "$attempts" -eq 100 ]; then
      echo 'fake networkd did not become ready' >&2
      exit 1
    fi
    sleep 0.01
  done
  NETCONF_TEST_PATH=$configuration
  cc -std=c11 -D_POSIX_C_SOURCE=200809L -D_DEFAULT_SOURCE -I. \
    -Iuserland/base/libedit -Wall -Wextra -Werror \
    -DNETWORKD_SOCKET="\"$socket_path\"" \
    -DNETCONF_PATH="\"$NETCONF_TEST_PATH\"" \
    userland/base/net/main.c userland/base/net/netconf.c \
    userland/base/net/protocol.c \
    userland/base/net/wifi-conf.c userland/base/net/wifi-store.c \
    userland/base/libedit/readline.c -o "$binary"
  "$binary" boot
  wait "$server_pid"
  server_pid=
}

run_case plan/ws011-net-config/tests/netconf-static.conf \
  'V1 UP em0
' \
  'V1 STATIC em0 ipv4 10.0.0.100 netmask 255.255.0.0
' \
  'V1 DEFAULTROUTE 10.0.0.1
' \
  'V1 DNS 10.0.0.1 10.0.0.2
'
run_case plan/ws011-net-config/tests/netconf-dhcp.conf \
  'V1 UP ne0
' \
  'V1 DHCP ne0 17
'

echo 'WS011 net boot application: PASS'

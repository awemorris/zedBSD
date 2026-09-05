#!/bin/sh
set -eu

test_root=${TMPDIR:-/tmp}/ws011-confirmed-commit.$$
socket_path=$test_root/networkd.sock
configuration=$test_root/net.conf
original=$test_root/net.conf.original
binary=$test_root/net
server=$test_root/networkd-fake
output=$test_root/output
errors=$test_root/errors
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
  -DNETCONF_PATH="\"$configuration\"" \
  -DNETCONF_LOCK_PATH="\"$test_root/net.conf.lock\"" \
  userland/base/net/main.c userland/base/net/netconf.c \
  userland/base/net/reconcile.c userland/base/net/protocol.c \
  userland/base/net/wifi-conf.c userland/base/net/wifi-store.c \
  userland/base/service/service-config.c userland/base/libedit/readline.c \
  -o "$binary"
cc -std=c11 -D_POSIX_C_SOURCE=200809L -Wall -Wextra -Werror \
  -I. plan/ws011-net-config/tests/networkd-fake.c \
  userland/base/net/protocol.c -o "$server"

rollback_program='V1 DEFAULTROUTE_CLEAR
V1 DNS_CLEAR
V1 UP em0
V1 STATIC em0 ipv4 10.0.0.100 netmask 255.255.0.0
V1 DEFAULTROUTE_CLEAR
V1 DEFAULTROUTE 10.0.0.1
V1 DNS 10.0.0.1 10.0.0.2'

check='V1 CHECK
'
arm='V1 ARM 1
'
clear_route='V1 DEFAULTROUTE_CLEAR token 305419896
'
clear_dns='V1 DNS_CLEAR token 305419896
'
up='V1 UP em0 token 305419896
'
static='V1 STATIC em0 ipv4 10.0.0.101 netmask 255.255.0.0 token 305419896
'
route='V1 DEFAULTROUTE 10.0.0.1 token 305419896
'
dns='V1 DNS 10.0.0.1 10.0.0.2 token 305419896
'
token_check='V1 CHECK token 305419896
'
disarm='V1 DISARM token 305419896
'
rollback='V1 ROLLBACK
'

start_server()
{
  rm -f "$socket_path"
  NETWORKD_FAKE_ROLLBACK=$rollback_program \
  NETWORKD_FAKE_NETCONF=$configuration \
  NETWORKD_FAKE_ARM_CONFIG_CONTAINS=10.0.0.100 \
  NETWORKD_FAKE_DISARM_CONFIG_CONTAINS=10.0.0.101 \
  NETWORKD_FAKE_REQUEST_CONFIG_CONTAINS=10.0.0.100 \
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
}

finish_server()
{
  wait "$server_pid"
  server_pid=
}

seed_configuration()
{
  cp plan/ws011-net-config/tests/netconf-static.conf "$configuration"
  cp "$configuration" "$original"
  rm -f "$test_root/net.conf.lock"
}

edit_input()
{
  printf '%s\n' configure 'interface em0' \
    'static ipv4 10.0.0.101 prefix-length 16' exit \
    "$@"
}

session_input()
{
  edit_input 'commit confirmed 1' "$@"
}

plain_clear_route='V1 DEFAULTROUTE_CLEAR
'
plain_clear_dns='V1 DNS_CLEAR
'
plain_up='V1 UP em0
'
plain_static='V1 STATIC em0 ipv4 10.0.0.101 netmask 255.255.0.0
'
plain_old_static='V1 STATIC em0 ipv4 10.0.0.100 netmask 255.255.0.0
'
plain_route='V1 DEFAULTROUTE 10.0.0.1
'
plain_dns='V1 DNS 10.0.0.1 10.0.0.2
'

# A normal commit reconciles first and publishes only after runtime succeeds.
seed_configuration
start_server "$check" "$plain_clear_route" "$plain_clear_dns" \
  "$plain_up" "$plain_static" "$plain_clear_route" "$plain_route" "$plain_dns"
edit_input commit end exit | "$binary" >"$output" 2>"$errors"
finish_server
grep -q 'Commit complete.' "$output"
grep -q 'address: 10.0.0.101' "$configuration"

# A partial normal apply restores the complete old intent and never publishes.
seed_configuration
start_server "$check" "$plain_clear_route" "$plain_clear_dns" \
  "$plain_up" "FAIL $plain_static" \
  "$plain_clear_route" "$plain_clear_dns" "$plain_up" "$plain_old_static" \
  "$plain_clear_route" "$plain_route" "$plain_dns"
edit_input commit end exit y | "$binary" >"$output" 2>"$errors"
finish_server
grep -q 'commit apply failed' "$errors"
cmp "$original" "$configuration"

# The confirming commit publishes only after both complete reconciliations and
# sends the matching-token disarm last.
seed_configuration
start_server "$check" "$arm" \
  "$clear_route" "$clear_dns" "$up" "$static" "$clear_route" "$route" "$dns" \
  "$token_check" \
  "$clear_route" "$clear_dns" "$up" "$static" "$clear_route" "$route" "$dns" \
  "$disarm"
session_input commit end exit | "$binary" >"$output" 2>"$errors"
finish_server
grep -q 'Confirmed commit applied; rollback is armed for 1 minute.' "$output"
grep -q 'Commit complete.' "$output"
grep -q 'address: 10.0.0.101' "$configuration"

# Explicit rollback does not publish the candidate and reloads the old bytes.
seed_configuration
start_server "$check" "$arm" \
  "$clear_route" "$clear_dns" "$up" "$static" "$clear_route" "$route" "$dns" \
  "$rollback"
session_input rollback end exit | "$binary" >"$output" 2>"$errors"
finish_server
cmp "$original" "$configuration"

# Losing the originating session cannot publish or disarm its candidate.
seed_configuration
start_server "$check" "$arm" \
  "$clear_route" "$clear_dns" "$up" "$static" "$clear_route" "$route" "$dns"
session_input end exit y | "$binary" >"$output" 2>"$errors"
finish_server
cmp "$original" "$configuration"

# Three lost disarm acknowledgements produce a nonzero uncertain outcome after
# publication; they are bounded rather than retried forever.
seed_configuration
start_server "$check" "$arm" \
  "$clear_route" "$clear_dns" "$up" "$static" "$clear_route" "$route" "$dns" \
  "$token_check" \
  "$clear_route" "$clear_dns" "$up" "$static" "$clear_route" "$route" "$dns" \
  "LOSE $disarm" "LOSE $disarm" "LOSE $disarm"
set +e
session_input commit end exit | "$binary" >"$output" 2>"$errors"
status=$?
set -e
finish_server
if [ "$status" -eq 0 ]; then
  echo 'lost disarm acknowledgements returned success' >&2
  exit 1
fi
grep -q 'outcome uncertain after publishing' "$errors"
grep -q 'address: 10.0.0.101' "$configuration"

echo 'WS011 confirmed commit integration: PASS'

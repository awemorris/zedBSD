#!/bin/sh
set -eu

repo=$(CDPATH= cd -- "$(dirname -- "$0")/../../.." && pwd)
base=${TMPDIR:-$repo/plan/ws005-networking/temp}
mkdir -p "$base"
test_root=$(mktemp -d "$base/wifi-conf-store.XXXXXX")
trap 'rm -rf -- "$test_root"' EXIT HUP INT TERM

common='-std=c11 -D_POSIX_C_SOURCE=200809L -D_DEFAULT_SOURCE -DWIFI_STORE_TESTING -I. -Wall -Wextra -Werror'
sources='userland/base/net/wifi-conf.c userland/base/net/wifi-store.c plan/ws005-networking/tests/wifi-store-test.c'

cd "$repo"
cc $common $sources -o "$test_root/store-test"
mkdir -m 700 "$test_root/ordinary"
"$test_root/store-test" "$test_root/ordinary"

cc $common -O1 -g -fno-omit-frame-pointer \
  -fsanitize=address,undefined $sources -o "$test_root/store-test-sanitize"
mkdir -m 700 "$test_root/sanitize"
ASAN_OPTIONS=detect_leaks=0 WIFI_STORE_SKIP_SLOW=1 \
  "$test_root/store-test-sanitize" \
  "$test_root/sanitize"

cc $common -fanalyzer $sources -o "$test_root/store-test-analyzer"
mkdir -m 700 "$test_root/analyzer"
WIFI_STORE_SKIP_SLOW=1 "$test_root/store-test-analyzer" "$test_root/analyzer"

cc -std=c11 -D_DEFAULT_SOURCE -I. -Wall -Wextra -Werror \
  userland/base/net/wifi-conf.c \
  plan/ws005-networking/tests/wifi-conf-model-test.c \
  -o "$test_root/model-test"
"$test_root/model-test"

echo 'WS005 wifi.conf store gates: PASS'

#!/bin/sh
set -eu
repo=$(CDPATH= cd -- "$(dirname -- "$0")/../../.." && pwd)
test_dir=$repo/plan/ws005/tests
work=${STORY_OUTPUT:-$repo/plan/ws005/temp/q085-wifi-scenarios/stories}
mkdir -p "$work"
cc=${CC:-cc}
variant=${STORY_VARIANT:-ordinary}
options='-O1 -g'
if [ "$variant" = sanitize ]; then
 options="$options -fsanitize=address,undefined -fno-omit-frame-pointer"
fi
# The overlay selects target network structs, while host process/file/time APIs
# and errno remain native. No production policy or child-stream parser is mocked.
$cc -std=c11 -D_GNU_SOURCE -DKERN_USER_ABI_LP64 -Wall -Wextra -Werror \
 -ffunction-sections -fdata-sections $options \
 -I"$test_dir/story-include" -I"$repo/include/uapi" -I"$repo" \
 "$test_dir/wifi-story-test.c" "$test_dir/wifi-story-world.c" \
 "$test_dir/wifi-story-net.c" "$test_dir/wifi-story-daemon.c" \
 "$test_dir/wifi-story-wifi.c" "$test_dir/wifi-story-child.c" \
 "$test_dir/wifi-story-netutil.c" \
 "$repo/userland/base/net/protocol.c" "$repo/userland/base/net/wifi-conf.c" \
 "$repo/userland/base/networkd/managed-wlan.c" "$repo/userland/base/networkd/confirmed.c" \
 -Wl,--gc-sections -o "$work/$variant"
ASAN_OPTIONS=detect_leaks=1 UBSAN_OPTIONS=halt_on_error=1 "$work/$variant"

#!/bin/sh
set -eu
repo=$(CDPATH= cd -- "$(dirname -- "$0")/../../.." && pwd)
out=$(mktemp -d "${TMPDIR:-/tmp}/zedbsd-nvme-registry.XXXXXXXX")
trap 'rm -rf "$out"' EXIT HUP INT TERM
for mode in normal sanitize; do
 flags=''
 if [ "$mode" = sanitize ]; then flags='-fsanitize=address,undefined -fno-omit-frame-pointer --param=asan-globals=0'; fi
 cc -std=c11 -pthread -O1 -g -Dtid_t=int32_t -DHAL_ARCH_AMD64 -DZEDBSD_USER_ABI_LP64 \
  -I"$repo" -I"$repo/include" -I"$repo/include/uapi" -Wall -Wextra -Werror \
  -ffunction-sections -fdata-sections $flags \
  "$repo/plan/ws004-hardware/tests/nvme-registry-host.c" -Wl,--gc-sections -o "$out/$mode"
 ASAN_OPTIONS=detect_leaks=0 "$out/$mode"
done

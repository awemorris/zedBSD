#!/bin/sh
# ws035-p007: compile the production HD Audio driver against a register and
# codec model, once normally and once with ASan/UBSan.
set -eu

repo=$(CDPATH= cd -- "$(dirname -- "$0")/../../.." && pwd)
work=$(mktemp -d "${TMPDIR:-/tmp}/ws035-hda.XXXXXX")
trap 'rm -rf -- "$work"' EXIT HUP INT TERM

flags="-std=c11 -Wall -Wextra -Werror -DKERN_USER_ABI_LP64 -DHAL_ARCH_AMD64
    -I$repo/include -I$repo/src -I$repo/include/libc -DKERN_UAPI_NATIVE
    -include $repo/include/libc/sys/ioctl.h"

# shellcheck disable=SC2086
cc $flags -O2 "$repo/plan/ws035/tests/hda-fixture.c" \
    "$repo/src/drivers/pci/pci-hda.c" -o "$work/ordinary"
"$work/ordinary"

# shellcheck disable=SC2086
cc $flags -O1 -g -fsanitize=address,undefined -fno-omit-frame-pointer \
    "$repo/plan/ws035/tests/hda-fixture.c" \
    "$repo/src/drivers/pci/pci-hda.c" -o "$work/sanitized"
ASAN_OPTIONS=detect_leaks=1 UBSAN_OPTIONS=halt_on_error=1 "$work/sanitized"

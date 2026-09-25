#!/bin/sh
# ws035-p006: compile the production audio framework against a host fixture
# with a fake driver, once normally and once with ASan/UBSan.
set -eu

repo=$(CDPATH= cd -- "$(dirname -- "$0")/../../.." && pwd)
work=$(mktemp -d "${TMPDIR:-/tmp}/ws035-audio.XXXXXX")
trap 'rm -rf -- "$work"' EXIT HUP INT TERM

# The UAPI layout is the same for ILP32 and LP64 callers.
for abi in 32 64; do
	cc -std=c89 -m$abi -ffreestanding -Wall -Wextra -Werror \
	    -I"$repo/include" -I"$repo/include/libc" -DKERN_UAPI_NATIVE \
	    -c "$repo/plan/ws035/tests/audio-uapi-layout.c" -o "$work/layout$abi.o"
done
echo "audio UAPI: ILP32/LP64 sizes and ioctl encoding PASS"

flags="-std=c11 -Wall -Wextra -Werror -DKERN_USER_ABI_LP64 -DHAL_ARCH_AMD64 -DKERN_KCRT_NATIVE
    -I$repo/include -I$repo/src -I$repo/include/libc -DKERN_UAPI_NATIVE
    -include $repo/include/libc/sys/ioctl.h"

# shellcheck disable=SC2086
cc $flags -O2 "$repo/plan/ws035/tests/audio-framework.c" \
    "$repo/src/drivers/audio/audio.c" -o "$work/ordinary"
"$work/ordinary"

# shellcheck disable=SC2086
cc $flags -O1 -g -fsanitize=address,undefined -fno-omit-frame-pointer \
    "$repo/plan/ws035/tests/audio-framework.c" \
    "$repo/src/drivers/audio/audio.c" -o "$work/sanitized"
ASAN_OPTIONS=detect_leaks=1 UBSAN_OPTIONS=halt_on_error=1 "$work/sanitized"

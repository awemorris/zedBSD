#!/bin/sh
# zedBSD; Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
# Compare C/C++ platform declarations on both x86 models without executing target code.
set -eu
repo=$(CDPATH= cd -- "$(dirname -- "$0")/../../../.." && pwd)
cd "$repo"
reference_core=${1:-/tmp/q308-virglrenderer-1.1.0/src/venus/venus-protocol}
reference_wayland=${2:-/tmp/q309-vulkan-wayland-1.3.269.h}
test "$(sha256sum "$reference_core/vulkan_core.h" | cut -d ' ' -f 1)" = 8cb01233ecf0fac130f0db2fdbbd79b6643f91f65c9afb807978924148798287
test "$(sha256sum "$reference_wayland" | cut -d ' ' -f 1)" = 3728578b8d6d98f6f3d20672406f869253eeacae8546a9d9577bca5c63a88d12
work=$(mktemp -d /tmp/zedbsd-wayland-abi.XXXXXXXX)
trap 'rm -rf "$work"' EXIT HUP INT TERM
mkdir -p "$work/maintained" "$work/reference/vulkan"
ln -s "$repo/libc/include/vulkan" "$work/maintained/vulkan"
ln -s "$reference_core/vulkan_core.h" "$work/reference/vulkan/vulkan_core.h"
ln -s "$reference_core/vk_platform.h" "$work/reference/vulkan/vk_platform.h"
ln -s "$reference_wayland" "$work/reference/vulkan/vulkan_wayland.h"
printf '#include "vulkan_core.h"\n#include "vulkan_wayland.h"\n' > "$work/reference/vulkan/vulkan.h"
for target in i386-unknown-zedbsd x86_64-unknown-zedbsd; do
    for language in c c++; do
        standard=c11
        if [ "$language" = c++ ]; then standard=c++11; fi
        build/llvm/bin/clang --target="$target" -x "$language" -std="$standard" \
            -ffreestanding -nostdinc -Wall -Wextra -Werror -Ilibc/include -DKERN_UAPI_NATIVE -Iinclude \
            -fsyntax-only plan/ws014/phase006/tests/wayland-abi.c
        for variant in maintained reference; do
            build/llvm/bin/clang --target="$target" -x "$language" -std="$standard" \
                -ffreestanding -nostdinc -Wall -Wextra -Werror -Wno-comment \
                -I"$work/$variant" -I"$reference_core" -Ilibc/include -DKERN_UAPI_NATIVE -I/usr/include \
                -c plan/ws014/phase006/tests/vulkan-wayland-abi.c -o "$work/$variant.o"
            build/llvm/bin/llvm-objcopy --dump-section .rodata="$work/$variant.bin" \
                "$work/$variant.o"
        done
        cmp "$work/maintained.bin" "$work/reference.bin"
        printf 'Wayland client and Vulkan platform ABI %s %s: PASS\n' "$target" "$language"
    done
done

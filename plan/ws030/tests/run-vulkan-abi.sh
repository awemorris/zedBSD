#!/bin/sh
# Compare maintained declarations with the exact pinned independent reference.
set -eu
root=$(CDPATH= cd -- "$(dirname -- "$0")/../../.." && pwd)
reference=${1:-/tmp/q308-virglrenderer-1.1.0/src/venus/venus-protocol}
actual=$(sha256sum "$reference/vulkan_core.h" | cut -d ' ' -f 1)
test "$actual" = 8cb01233ecf0fac130f0db2fdbbd79b6643f91f65c9afb807978924148798287
work=$(mktemp -d /tmp/zedbsd-vulkan-abi.XXXXXX)
trap 'rm -rf -- "$work"' EXIT HUP INT TERM
mkdir "$work/maintained" "$work/reference"
ln -s "$root/libc/include/vulkan" "$work/maintained/vulkan"
ln -s "$reference" "$work/reference/vulkan"
cd "$root"
timeout 90 build/NoctLang/build-static/noct plan/ws030/tests/vulkan-abi-source.noct \
    libc/include/vulkan/vulkan_core.h "$work/measure.c"
for target in i386-unknown-elf x86_64-unknown-elf; do
    for variant in maintained reference; do
        build/llvm/bin/clang --target="$target" -ffreestanding -nostdinc \
            -std=c89 -Wall -Wextra -Werror -Wno-comment -I"$work/$variant" -Ilibc/include -I/usr/include \
            -c "$work/measure.c" -o "$work/$variant.o"
        build/llvm/bin/llvm-objcopy --dump-section .rodata="$work/$variant.bin" \
            "$work/$variant.o"
    done
    cmp "$work/maintained.bin" "$work/reference.bin"
    printf 'Vulkan ABI %s: exact layouts and constants PASS\n' "$target"
done

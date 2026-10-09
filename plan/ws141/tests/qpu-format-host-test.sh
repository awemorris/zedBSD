#!/bin/sh
# Checks native words with the unmodified fixed MIT Mesa oracle in ignored temp.
# Usage: sh plan/ws141/tests/qpu-format-host-test.sh [output-directory]
set -eu
out=${1:-build/ws141-qpu-format-host}
mesa=plan/ws141/temp/mesa/src
mkdir -p "$out/mesa-host/util"
# Minimal standard helper declarations replace build-system-only Mesa util dependencies.
# The actual instruction encoder, decoder, enums and device description remain unmodified.
cat > "$out/mesa-host/util/macros.h" <<'HEADER'
#ifndef WS141_MESA_HOST_MACROS_H
#define WS141_MESA_HOST_MACROS_H
#include <assert.h>
#include <stdint.h>
#include <stdlib.h>
#define ARRAY_SIZE(a) (sizeof(a) / sizeof((a)[0]))
#define STATIC_ASSERT(e) _Static_assert(e, #e)
#define BITFIELD64_BIT(n) (UINT64_C(1) << (n))
#define BITFIELD64_RANGE(b,n) (((n) == 64 ? UINT64_MAX : ((UINT64_C(1) << (n)) - 1)) << (b))
#define UNREACHABLE(reason) abort()
#define ATTRIBUTE_CONST __attribute__((const))
#define ATTRIBUTE_PURE __attribute__((pure))
#endif
HEADER
cat > "$out/mesa-host/util/bitscan.h" <<'HEADER'
#ifndef WS141_MESA_HOST_BITSCAN_H
#define WS141_MESA_HOST_BITSCAN_H
#include <strings.h>
#endif
HEADER
sha256sum -c <<HASHES
abf436006dd3bed52cf1e77245d7267105a9100f02d8b3d643ddc6641fd6d551  $mesa/broadcom/qpu/qpu_pack.c
4b1b90e3bb8ea614dea36484e24d83049392c55cf19b01125627fa5ff4c9d07f  $mesa/broadcom/qpu/qpu_instr.h
f61745f35e5b34c74aad56b01afb9e068860ecb067361b8f9610dc1b673c8d4e  $mesa/broadcom/qpu/qpu_instr.c
HASHES
cc -std=gnu11 -Wall -Werror -I"$out/mesa-host" -I"$mesa" \
    -c "$mesa/broadcom/qpu/qpu_pack.c" -o "$out/mesa-pack.o"
cc -std=gnu11 -Wall -Werror -I"$out/mesa-host" -I"$mesa" \
    -c "$mesa/broadcom/qpu/qpu_instr.c" -o "$out/mesa-instr.o"
cc -std=c99 -Wall -Wextra -Werror -Iinclude -Isrc -I"$out/mesa-host" -I"$mesa" \
    plan/ws141/tests/qpu-format-host-test.c src/drivers/gpu/bcm2711/qpu.c \
    "$out/mesa-pack.o" "$out/mesa-instr.o" -o "$out/qpu-format-host-test"
"$out/qpu-format-host-test"

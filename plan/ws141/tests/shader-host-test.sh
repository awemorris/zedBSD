#!/bin/sh
# Actual SPIR-V/native scalar differential check; physical QPU, texture memory and scheduling are untested.
# Usage: sh plan/ws141/tests/shader-host-test.sh [output-directory]
set -eu
out=${1:-build/ws141-shader-host}
mesa=plan/ws141/temp/mesa/src
# Reuses only the fixed independently hashed oracle object setup, not any production encoding expectation.
sh plan/ws141/tests/qpu-format-host-test.sh "$out"
cc -std=c99 -Wall -Wextra -Werror -Iinclude -Isrc -I. -I"$out/mesa-host" -I"$mesa" \
    plan/ws141/tests/shader-host-test.c src/drivers/gpu/compiler/spirv.c \
    src/drivers/gpu/bcm2711/shader.c src/drivers/gpu/bcm2711/shader-analyze.c \
    src/drivers/gpu/bcm2711/shader-lower.c src/drivers/gpu/bcm2711/shader-output.c \
    src/drivers/gpu/bcm2711/qpu.c "$out/mesa-pack.o" "$out/mesa-instr.o" \
    -lm -o "$out/shader-host-test"
"$out/shader-host-test"

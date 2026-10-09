#!/bin/sh
# Actual native buffer and retained VM mapping semantics; host aliases do not simulate physical cache/SMP.
# Usage: sh plan/ws141/tests/uncached-mapping-host-test.sh [output-directory]
set -eu
out=${1:-build/ws141-uncached-mapping-host}
mkdir -p "$out"
cc -std=c99 -D_POSIX_C_SOURCE=200809L -include time.h -Wall -Wextra -Werror \
    -Iplan/ws141/tests/host -Iinclude -Isrc -I. \
    plan/ws141/tests/uncached-mapping-host-test.c plan/ws141/tests/display-lock-host.c \
    src/drivers/gpu/bcm2711/buffer.c src/kern/vm-device.c \
    -o "$out/uncached-mapping-host-test"
"$out/uncached-mapping-host-test"

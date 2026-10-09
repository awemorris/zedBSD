#!/bin/sh
# WS141 initial display preparation; no physical GPU or QEMU is exercised.
# Usage: sh plan/ws141/tests/display-host-test.sh [output-directory]
set -eu
out=${1:-build/ws141-display-host}
mkdir -p "$out"
cc -std=c99 -Wall -Wextra -Werror -Iinclude -Isrc -I. \
    plan/ws141/tests/display-program-host-test.c \
    src/drivers/gpu/bcm2711/display-program.c src/drivers/gpu/bcm2711/list.c \
    -o "$out/display-program-host-test"
"$out/display-program-host-test"
cc -std=c99 -D_POSIX_C_SOURCE=200809L -include time.h -Wall -Wextra -Werror -Iinclude -Isrc -I. \
    plan/ws141/tests/display-execute-host-test.c \
    src/drivers/gpu/bcm2711/display-program.c src/drivers/gpu/bcm2711/display-execute.c \
    src/drivers/gpu/bcm2711/display-irq.c src/drivers/gpu/bcm2711/display-flip.c \
    plan/ws141/tests/display-lock-host.c \
    -o "$out/display-execute-host-test"
"$out/display-execute-host-test"
cc -std=c99 -D_POSIX_C_SOURCE=200809L -include time.h -Wall -Wextra -Werror -Iinclude -Isrc -I. \
    plan/ws141/tests/display-flip-host-test.c plan/ws141/tests/display-lock-host.c \
    src/drivers/gpu/bcm2711/display-flip.c src/drivers/gpu/bcm2711/display-irq.c \
    -o "$out/display-flip-host-test"
"$out/display-flip-host-test"
cc -std=c99 -D_POSIX_C_SOURCE=200809L -include time.h -Wall -Wextra -Werror -Iinclude -Isrc -I. \
    plan/ws141/tests/display-device-host-test.c plan/ws141/tests/display-lock-host.c \
    src/drivers/gpu/bcm2711/buffer.c src/drivers/gpu/bcm2711/display-device.c \
    src/drivers/gpu/bcm2711/display-flip.c src/drivers/gpu/bcm2711/display-irq.c \
    -o "$out/display-device-host-test"
"$out/display-device-host-test"

#!/bin/sh
# Actual provider startup/reset with the fixed firmware DTB, never real MMIO.
# Usage: sh plan/ws141/tests/v3d-power-host-test.sh [output-directory] [fixed-dtb]
set -eu
out=${1:-build/ws141-v3d-power-host}
dtb=${2:-plan/ws141/temp/fwdtb/bcm2711-rpi-4-b.dtb}
mkdir -p "$out"
python3 - "$dtb" <<'PY'
import hashlib,sys
from pathlib import Path
assert hashlib.sha256(Path(sys.argv[1]).read_bytes()).hexdigest() == '75761b73c284e26623e4d1624bff13e67bce2ae620880efd81d6571a3739fcfb'
PY
cc -std=c99 -D_POSIX_C_SOURCE=200809L -include time.h -Wall -Wextra -Werror \
    -ffunction-sections -Wl,--gc-sections -Iinclude -Isrc -I. \
    plan/ws141/tests/v3d-power-host-test.c src/drivers/gpu/bcm2711/v3d-power.c \
    src/drivers/generic/fdt.c src/drivers/gpu/bcm2711/fdt-util.c \
    -o "$out/v3d-power-host-test"
"$out/v3d-power-host-test" "$dtb"

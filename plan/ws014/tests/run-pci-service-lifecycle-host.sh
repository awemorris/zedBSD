#!/bin/sh
set -eu

repository=$(CDPATH= cd -- "$(dirname -- "$0")/../../.." && pwd)
# The build folder is kept (the first argument, default build/ws014-pci-service).
workspace=${1:-"$repository/build/ws014-pci-service"}
mkdir -p "$workspace"
compiler=${CC:-cc}

# Links the actual PCI implementation and discards unrelated IRQ entry points.
"$compiler" -std=gnu11 -Wall -Wextra -Werror -O1 -g \
    -ffunction-sections -fdata-sections -I"$repository/include" \
    "$repository/plan/ws014/tests/pci-service-lifecycle-host.c" \
    "$repository/src/drivers/pci/pci.c" -Wl,--gc-sections \
    -o "$workspace/pci-service"
"$workspace/pci-service"

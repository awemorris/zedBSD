#!/bin/sh
set -eu

repository=$(CDPATH= cd -- "$(dirname -- "$0")/../../.." && pwd)
workspace=$(mktemp -d "${TMPDIR:-/tmp}/zedbsd-pci-service.XXXXXX")
trap 'rm -rf "$workspace"' EXIT HUP INT TERM
compiler=${CC:-cc}

# Links the actual PCI implementation and discards unrelated IRQ entry points.
"$compiler" -std=gnu11 -Wall -Wextra -Werror -O1 -g \
    -ffunction-sections -fdata-sections -I"$repository/include" \
    "$repository/plan/ws014/tests/pci-service-lifecycle-host.c" \
    "$repository/src/drivers/pci/pci.c" -Wl,--gc-sections \
    -o "$workspace/pci-service"
"$workspace/pci-service"

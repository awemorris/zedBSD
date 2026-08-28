#!/bin/sh
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
set -eu

repo=$(CDPATH= cd -- "$(dirname -- "$0")/../../.." && pwd)
temporary=$(mktemp -d "${TMPDIR:-/tmp}/zedbsd-net-hotplug.XXXXXX")
trap 'rm -rf "$temporary"' EXIT HUP INT TERM

cc -std=c11 -D_GNU_SOURCE -Wall -Wextra -Werror -ffunction-sections -pthread \
	-I"$repo/include" -I"$repo/include/uapi" -I"$repo/src/kern/net" \
    "$repo/src/kern/net/net-device.c" \
    "$repo/src/kern/net/route.c" \
    "$repo/plan/ws004-hardware/tests/net-device-hotplug-test.c" \
	-Wl,--gc-sections -pthread -o "$temporary/net-device-hotplug-test"
"$temporary/net-device-hotplug-test"

cc -std=c11 -D_GNU_SOURCE -Wall -Wextra -Werror -ffunction-sections \
	-I"$repo/plan/ws004-hardware/tests/host-include" \
	-I"$repo/include" -I"$repo/include/uapi" -I"$repo/src/kern/net" \
	"$repo/src/kern/net/net-device.c" \
	"$repo/src/kern/net/arp.c" \
	"$repo/plan/ws004-hardware/tests/arp-hotplug-test.c" \
	-Wl,--gc-sections -o "$temporary/arp-hotplug-test"
"$temporary/arp-hotplug-test"

cc -std=c11 -D_GNU_SOURCE -Wall -Wextra -Werror -ffunction-sections \
	-I"$repo/include" -I"$repo/include/uapi" -I"$repo/src/kern/net" \
	"$repo/src/kern/net/net-device.c" \
	"$repo/src/kern/net/inet-socket.c" \
	"$repo/plan/ws004-hardware/tests/inet-interface-hotplug-test.c" \
	-Wl,--gc-sections -o "$temporary/inet-interface-hotplug-test"
"$temporary/inet-interface-hotplug-test"

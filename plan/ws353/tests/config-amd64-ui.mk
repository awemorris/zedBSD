# The x86_64 guest image of the compositor's UI deadline test (qemu-ui-deadlines.sh): the image of qemu-deadlines.sh,
# with the kernel's test input injector and its client aat-input (a mouse, a keyboard and a touch screen the test
# drives), the Settings application (a window with a titlebar search field and an icon in the bar), the printers'
# daemon (with it the compositor offers the printers, and Settings holds a printers object), and the guest's script
# that sums the timers fired by name.
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
include $(abspath $(dir $(lastword $(MAKEFILE_LIST))))/config-amd64-deadlines.mk

override CONFIG_INPUT_TEST_INJECT := y
override CONFIG_USERLAND_PROGRAMS += aat-input settings printd

WS353_HARNESS_FILES += \
	--file /root/perf-timers.sh=$(WS353_TREE)/plan/ws353/tests/perf-timers.sh

X86_64_ARCH_INPUTS += $(WS353_TREE)/plan/ws353/tests/config-amd64-ui.mk \
	$(WS353_TREE)/plan/ws353/tests/perf-timers.sh

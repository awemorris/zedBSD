# The x86_64 guest image of the compositor's deadline test (qemu-deadlines.sh): the image of qemu-main-wake.sh, with
# keiland-system (the system monitor's client) and a stand-in input method that ends at once
# (plan/ws353/tests/fake-ime.sh as /usr/libexec/keiland-ime).
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
include $(abspath $(dir $(lastword $(MAKEFILE_LIST))))/config-amd64-mainwake.mk

override CONFIG_USERLAND_PROGRAMS += keiland-system

WS353_HARNESS_FILES += \
	--file /usr/libexec/keiland-ime=$(WS353_TREE)/plan/ws353/tests/fake-ime.sh \
	--mode /usr/libexec/keiland-ime=0755

X86_64_ARCH_INPUTS += $(WS353_TREE)/plan/ws353/tests/config-amd64-deadlines.mk \
	$(WS353_TREE)/plan/ws353/tests/fake-ime.sh

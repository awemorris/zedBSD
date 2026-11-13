# The x86_64 guest image of the compositor's animation and clock test (qemu-animation.sh): the image of
# qemu-ui-deadlines.sh, with the notifications' test client keiland-notify (a board that enters, stays and leaves).
# It is a superset of the main-wake, deadlines and UI images, so those tests run on it too (IMAGE=).
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
include $(abspath $(dir $(lastword $(MAKEFILE_LIST))))/config-amd64-ui.mk

override CONFIG_USERLAND_PROGRAMS += keiland-notify

X86_64_ARCH_INPUTS += $(WS353_TREE)/plan/ws353/tests/config-amd64-anim.mk

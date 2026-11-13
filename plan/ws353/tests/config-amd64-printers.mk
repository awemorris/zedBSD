# The x86_64 guest image of the printers' test without a clock (qemu-printers.sh): the image of qemu-ui-deadlines.sh
# (Settings, the printers' daemon, the guest's timer and rate scripts), with printtest, the printers' command-line
# client of libkeiland.
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
include $(abspath $(dir $(lastword $(MAKEFILE_LIST))))/config-amd64-ui.mk

override CONFIG_USERLAND_PROGRAMS += printtest

X86_64_ARCH_INPUTS += $(WS353_TREE)/plan/ws353/tests/config-amd64-printers.mk

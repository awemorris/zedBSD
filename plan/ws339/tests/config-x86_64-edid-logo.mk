# config-x86_64-edid.mk with the boot splash (logo=logo.ppm in zedbsd.cfg), for p001-qemu.sh's logo cases: without an
# EDID the loader wishes for the splash's 1920x1080, with one it takes the monitor's size first.
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
TEST_TREE := $(abspath $(dir $(lastword $(MAKEFILE_LIST)))/../../..)
include $(TEST_TREE)/plan/ws339/tests/config-x86_64-edid.mk
override CONFIG_BOOT_SPLASH := y

# The x86_64 image of the scheduler hints' stress test (p003-qemu.sh): the base userland with the console mirrored on
# the serial port (plan/tools/posix/config-amd64-serial.mk), the kernel with the balancer and its stress test, and
# without the ISA NE2000 that configuration turns on (the guest has none).  The objects the balancer's proposed
# make rules add come from desktop-balancer-overlay.mk on the command line.
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
# The tree, found from this file: the packages' nested makes read the configuration from their own
# directories, so the files it includes are named by absolute path.
TEST_TREE := $(abspath $(dir $(lastword $(MAKEFILE_LIST)))/../../..)
include $(TEST_TREE)/plan/tools/posix/config-amd64-serial.mk
override CONFIG_DRIVER_NE2000 := n
override CONFIG_KERN_DESKTOP_BALANCER := y
override CONFIG_KERN_DESKTOP_BALANCER_TEST := y

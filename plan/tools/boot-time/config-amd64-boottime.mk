# The amd64 image whose boot is timed in QEMU (boot-time.py): the current UAT's image (config/current-uat.mk:
# graphical login, sshd on) without the target's clang, libc++ and Emacs (an implementation agent does not build
# the target toolchain), with root keeping the tree's password so that the guest harness can log in over SSH.
# BOOT_TIME_KMSG=console leaves the kernel's messages on the screen, as the Raspberry Pi images do; the default
# keeps them off (kmsg=quiet).  Build with the guest harness's files:
#   flock /tmp/zedbsd-image-build.lock plan/tools/guest/test-image.sh plan/tools/boot-time/config-amd64-boottime.mk BUILD
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
include config/current-uat.mk
ZEDBSD_USER_PROGRAMS := $(filter-out clang libcxx emacs,$(ZEDBSD_USER_PROGRAMS))
ZEDBSD_ROOT_LOCKED := n
ifeq ($(BOOT_TIME_KMSG),console)
ZEDBSD_BOOT_KERNEL_MESSAGES := y
endif

# zedBSD
# Copyright (C) 2026 Awe Morris
#
# SPDX-License-Identifier: Zlib

#
# The proposed CONFIG_KERN_DESKTOP_BALANCER rule of mk/kernel.mk and
# CONFIG_KERN_DESKTOP_BALANCER_TEST rule of mk/test-kernel.mk, given on the
# make command line until the proposals are applied, so that a kernel with
# the balancer (and its stress test) can be built without changing the
# build system:
#
#   make -f Makefile -f plan/ws302/tests/desktop-balancer-overlay.mk \
#	CONFIGMK=config/config.<platform>.mk CONFIG_KERN_DESKTOP_BALANCER=y \
#	[CONFIG_KERN_DESKTOP_BALANCER_TEST=y] build/<platform>/vmunix
#
# It is read after the Makefile, so the platform's object list and link
# rule already exist.  The link recipes expand VMUNIX_OBJS when they run,
# so the objects added here are linked; the flag stamp is written from the
# compile flags at run time, so every object is rebuilt when the options
# change.  Once mk/kernel.mk and mk/test-kernel.mk have the rules this file
# must not be used.
#

BALANCER_OVERLAY_OBJS :=
ifneq ($(filter y,$(CONFIG_KERN_DESKTOP_BALANCER)),)
KERN_CPPFLAGS += -DCONFIG_KERN_DESKTOP_BALANCER=1
BALANCER_OVERLAY_OBJS += $(BUILD)/src/kern/desktop-balancer.o
ifneq ($(filter y,$(CONFIG_KERN_DESKTOP_BALANCER_TEST)),)
KERN_CPPFLAGS += -DCONFIG_KERN_DESKTOP_BALANCER_TEST=1
BALANCER_OVERLAY_OBJS += $(BUILD)/src/kern/sched-hint-test.o
endif
endif
ifneq ($(BALANCER_OVERLAY_OBJS),)
VMUNIX_OBJS += $(BALANCER_OVERLAY_OBJS)
$(BUILD)/vmunix $(BUILD)/kernel.elf: $(BALANCER_OVERLAY_OBJS)
$(BALANCER_OVERLAY_OBJS): $(KERN_FLAGS_STAMP)
-include $(BALANCER_OVERLAY_OBJS:.o=.d)
endif

# zedBSD
# Copyright (C) 2026 Awe Morris
#
# SPDX-License-Identifier: Zlib

#
# The proposed CONFIG_DTB rule of mk/kernel.mk, given on the make command
# line until the proposal is applied, so that a CONFIG_DTB=y kernel can be
# built without changing the build system:
#
#   make -f Makefile -f plan/ws302/tests/dtb-overlay.mk \
#	CONFIGMK=config/config.<platform>.mk CONFIG_DTB=y build/<platform>/vmunix
#
# It is read after the Makefile, so the platform's object list and link
# rule already exist.  The link recipes expand VMUNIX_OBJS when they run,
# so the object added here is linked; the flag stamp is written from the
# compile flags at run time, so every object is rebuilt when CONFIG_DTB
# changes.  Once mk/kernel.mk has the rule this file must not be used.
#

ifneq ($(filter y,$(CONFIG_DTB)),)
KERN_CPPFLAGS += -DCONFIG_DTB=1
DTB_OVERLAY_OBJS := $(BUILD)/src/kern/dtb.o
VMUNIX_OBJS += $(DTB_OVERLAY_OBJS)
$(BUILD)/vmunix $(BUILD)/kernel.elf: $(DTB_OVERLAY_OBJS)
$(DTB_OVERLAY_OBJS): $(KERN_FLAGS_STAMP)
-include $(DTB_OVERLAY_OBJS:.o=.d)
endif

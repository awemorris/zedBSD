# Shared userland installation manifest; platform files own compilation.
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
# All platforms install the same selected commands, accounts and base data.
# A static-only ABI sets ZEDBSD_ROOTFS_DYNAMIC=n before including this file.
ZEDBSD_ROOTFS_DYNAMIC ?= y
ZEDBSD_ROOTFS_COMMANDS := $(sort sh sysctl mount umount \
 $(USERLAND_SELECTED_NETWORK_PROGRAMS) $(USERLAND_SELECTED_BASIC_PROGRAMS) \
 $(foreach program,$(filter $(ZEDBSD_USER_PROGRAMS),$(USERLAND_PACKAGES)),\
 $(if $(filter static,$(USERLAND_$(program)_CLASS)),$(program))))
ZEDBSD_ROOTFS_INPUTS := $(addprefix $(BUILD)/bin/,$(ZEDBSD_ROOTFS_COMMANDS)) \
 $(ZEDBSD_ACCOUNT_INPUTS) $(ZEDBSD_BASE_DATA_INPUTS)
ZEDBSD_ROOTFS_FILES := $(foreach command,$(ZEDBSD_ROOTFS_COMMANDS),\
 --file $(if $(filter umount,$(command)),/sbin/umount,$(call zedbsd_userland_destination,$(command)))=$(BUILD)/bin/$(command)) \
 $(ZEDBSD_USERLAND_FILE_MODES) $(ZEDBSD_ACCOUNT_FILES) $(ZEDBSD_BASE_DATA_FILES)

ifeq ($(ZEDBSD_ROOTFS_DYNAMIC),y)
# The destination names are part of the runtime ABI, shared by every CPU.
ZEDBSD_ROOTFS_RUNTIME := ld.so libc.so libutil.so tlstest.so \
 alt/rpathdep.so verstest.so versuse.so
ZEDBSD_ROOTFS_INPUTS += $(addprefix $(BUILD)/dynamic/,$(ZEDBSD_ROOTFS_RUNTIME)) \
 $(BUILD)/dynamic/rpathtest.so $(BUILD)/dynamic/dyntest
ZEDBSD_ROOTFS_FILES += $(foreach library,$(ZEDBSD_ROOTFS_RUNTIME),\
 --file /lib/$(library)=$(BUILD)/dynamic/$(library)) \
 --file /lib/rpthtest.so=$(BUILD)/dynamic/rpathtest.so \
 --file /bin/dyntest=$(BUILD)/dynamic/dyntest
endif

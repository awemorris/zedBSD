# zedBSD
# Copyright (C) 2026 Awe Morris
#
# SPDX-License-Identifier: Zlib

#
# Rebuild Tracking
#
# Objects follow the compile flags and the headers they include.  This file
# is read after the platform's makefiles, which define the object lists and
# the compile flags it refers to.
#

################################################
# Kernel
################################################

# The kernel objects follow the compile flags: a change of the
# configuration rebuilds them.  The stamp is written every time and
# replaced only when the flags change.
KERN_FLAGS_STAMP := $(BUILD)/.kernel-flags

# The kernel objects also follow the headers they include: the
# compiler writes each object's header list beside it, and the next
# run reads them.
KERN_CFLAGS += -MMD -MP

.PHONY: FORCE_KERN_FLAGS
FORCE_KERN_FLAGS:
$(KERN_FLAGS_STAMP): FORCE_KERN_FLAGS
	@mkdir -p $(dir $@)
	@printf '%s\n' '$(KERN_CPPFLAGS) $(KERN_CFLAGS)' > $@.tmp
	@if cmp -s $@.tmp $@; then rm -f $@.tmp; else mv -f $@.tmp $@; fi

$(VMUNIX_OBJS): $(KERN_FLAGS_STAMP)

-include $(VMUNIX_OBJS:.o=.d)

################################################
# libc
################################################

# Every libc object the platform's libc.mk lists: libc, libm, libutil,
# the compiler runtime and the LLVM builtins, static and dynamic.
LIBC_TRACKED_OBJS := $(sort $(filter $(LIBC_OBJ_DIR)/%.o,\
	$(foreach list,$(filter %_OBJS,$(.VARIABLES)),$($(list)))))

# The libc objects follow the compile flags, as the kernel's do.
LIBC_FLAGS_STAMP := $(BUILD)/.libc-flags
LIBC_TRACKED_FLAGS = $(foreach flags,$(sort $(filter %LIBC_STATIC_CFLAGS %LIBC_DYNAMIC_CFLAGS,$(.VARIABLES))),$($(flags)))

.PHONY: FORCE_LIBC_FLAGS
FORCE_LIBC_FLAGS:
$(LIBC_FLAGS_STAMP): FORCE_LIBC_FLAGS
	@mkdir -p $(dir $@)
	@printf '%s\n' '$(LIBC_TRACKED_FLAGS)' > $@.tmp
	@if cmp -s $@.tmp $@; then rm -f $@.tmp; else mv -f $@.tmp $@; fi

$(LIBC_TRACKED_OBJS): $(LIBC_FLAGS_STAMP)

# The libc objects also follow the headers they include.
$(LIBC_TRACKED_OBJS): CC += -MMD -MP

-include $(LIBC_TRACKED_OBJS:.o=.d)

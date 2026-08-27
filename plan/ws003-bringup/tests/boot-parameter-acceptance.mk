# BR-T46 test-only production-user-ABI helper build rules.
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib

BR_T46_SWAP_SOURCE := \
	plan/ws003-bringup/tests/boot-parameter-swap-exercise.c
BR_T46_SWAP_ELF := $(BUILD)/BR-T46-SWAP.ELF

# A generated acceptance configuration is also an input to the staged rootfs.
# The production rules otherwise see only the selected binaries, so an existing
# stamp from a different package selection could be reused accidentally.
$(BUILD)/rootfs/.stamp: $(ZEDBSD_CONFIG)

# The production architecture UFS targets are otherwise keyed only by their
# selected binaries.  Record the generated configuration itself so a later
# harness invocation cannot reuse an image assembled for another selection.
ifneq ($(filter pcat pc98,$(ZEDBSD_PLATFORM_DIR)),)
$(I386_ARCH_UFS_IMAGE): $(ZEDBSD_CONFIG)
else ifeq ($(ZEDBSD_PLATFORM_DIR),amd64)
$(AMD64_ARCH_UFS_IMAGE): $(ZEDBSD_CONFIG)
endif

ifeq ($(ZEDBSD_PLATFORM_DIR),amd64)
BR_T46_SWAP_OBJECT := $(BUILD)/user64/$(BR_T46_SWAP_SOURCE:.c=.o)

$(BR_T46_SWAP_OBJECT): $(BR_T46_SWAP_SOURCE)
	@mkdir -p $(dir $@)
	$(CC) $(AMD64_USER_CPPFLAGS) $(AMD64_USER_CFLAGS) \
		-fno-strict-aliasing -MMD -MP -c $< -o $@

$(BR_T46_SWAP_ELF): $(AMD64_USER_LIBC_OBJS) $(BR_T46_SWAP_OBJECT) \
	$(AMD64_PLATFORM)/user.ld $(AMD64_USER_ELF_CHECK)
	$(LD) -m elf_x86_64 --gc-sections -nostdlib -static \
		-z max-page-size=4096 -z stack-size=0x100000 \
		-T $(AMD64_PLATFORM)/user.ld $(AMD64_USER_LIBC_OBJS) \
		$(BR_T46_SWAP_OBJECT) -o $@
	@test -z "$$(nm -u $@)" || { nm -u $@; exit 1; }
	$(NOCT) --path=tools/build $(AMD64_USER_ELF_CHECK) \
		--machine amd64 $@
else ifneq ($(filter pcat pc98,$(ZEDBSD_PLATFORM_DIR)),)
BR_T46_SWAP_OBJECT := $(BUILD)/$(BR_T46_SWAP_SOURCE:.c=.o)
BR_T46_SWAP_LINKER := $(if $(filter pc98,$(ZEDBSD_PLATFORM_DIR)),\
	$(PC98)/noct-user.ld,$(PCAT)/user.ld)

$(BR_T46_SWAP_OBJECT): $(BR_T46_SWAP_SOURCE)
	@mkdir -p $(dir $@)
	$(CC) $(ZEDBSD_CPPFLAGS) $(USER_CFLAGS) -MMD -MP -c $< -o $@

$(BR_T46_SWAP_ELF): $(USER_LIBC_OBJS) $(BR_T46_SWAP_OBJECT) \
	$(ZEDBSD_SOFTFLOAT_OBJECTS) $(BR_T46_SWAP_LINKER) $(USER_ELF_CHECK)
	$(LD) -m elf_i386 --gc-sections -nostdlib -static \
		-z max-page-size=4096 $(USER_STACK_LDFLAGS) \
		-T $(BR_T46_SWAP_LINKER) $(USER_LIBC_OBJS) \
		$(BR_T46_SWAP_OBJECT) $(ZEDBSD_SOFTFLOAT_OBJECTS) -o $@
	@test -z "$$(nm -u $@)" || { nm -u $@; exit 1; }
	$(NOCT) --path=$(BUILD_TOOLS_DIR) $(USER_ELF_CHECK) $@
endif

.PHONY: br-t46-swap-exercise
br-t46-swap-exercise: $(BR_T46_SWAP_ELF)

# WS016-p004 test-only production-user-ABI helper build rules.
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib

WS016_RUNTIME_SWAP_SOURCE := \
	plan/ws016-swap-control/tests/runtime-swap-guest.c
WS016_RUNTIME_SWAP_OBJECT := \
	$(BUILD)/user64/$(WS016_RUNTIME_SWAP_SOURCE:.c=.o)
WS016_RUNTIME_SWAP_ELF := $(BUILD)/WS016-SWAP.ELF

# Do not reuse a rootfs or architecture UFS assembled under a different
# generated package configuration.  This is the same dependency fence used
# by BR-T46, kept here so the p004 makefile is safe when invoked on its own.
$(BUILD)/rootfs/.stamp: $(ZEDBSD_CONFIG)

ifeq ($(ZEDBSD_PLATFORM_DIR),amd64)
$(AMD64_ARCH_UFS_IMAGE): $(ZEDBSD_CONFIG)

$(WS016_RUNTIME_SWAP_OBJECT): $(WS016_RUNTIME_SWAP_SOURCE)
	@mkdir -p $(dir $@)
	$(CC) $(AMD64_USER_CPPFLAGS) $(AMD64_USER_CFLAGS) \
		-fno-strict-aliasing -MMD -MP -c $< -o $@

$(WS016_RUNTIME_SWAP_ELF): $(AMD64_USER_LIBC_OBJS) \
	$(WS016_RUNTIME_SWAP_OBJECT) $(AMD64_PLATFORM)/user.ld \
	$(AMD64_USER_ELF_CHECK)
	$(LD) -m elf_x86_64 --gc-sections -nostdlib -static \
		-z max-page-size=4096 -z stack-size=0x100000 \
		-T $(AMD64_PLATFORM)/user.ld $(AMD64_USER_LIBC_OBJS) \
		$(WS016_RUNTIME_SWAP_OBJECT) -o $@
	@test -z "$$(nm -u $@)" || { nm -u $@; exit 1; }
	$(NOCT) --path=tools/build $(AMD64_USER_ELF_CHECK) \
		--machine amd64 $@
endif

.PHONY: ws016-runtime-swap-exercise
ws016-runtime-swap-exercise: $(WS016_RUNTIME_SWAP_ELF)

# zedBSD
# Copyright (C) 2026 Awe Morris
#
# SPDX-License-Identifier: Zlib

#
# The 68040 Macintosh kernel, built with GCC (mk/gcc/).
#
# The kernel reads the C library's freestanding headers from the tree
# (include/libc), not from a sysroot: the platform has no C library yet.
# It uses no floating point, and the compiler's helpers for what the
# processor has no instruction for (64-bit division, bit counts) come from
# GCC's libgcc for the 68040 without a floating-point unit.
#

KERN_CPPFLAGS := \
	-nostdinc \
	-Iinclude/libc \
	-Iinclude \
	-Isrc \
	-I. \
	-D__ZEDBSD__ \
	-DHAL_ARCH_M68K \
	-DHAL_ARCH_M68040 \
	-DHAL_BOARD_MAC68K \
	-DKERN_USER_ABI_ILP32 \
	-DKERN_UAPI_NATIVE \
	-DKERN_ATOMIC_SERIALIZED \
	-DZEDBSD_VERSION=\"$(VERSION)\" \
	$(KERN_CPPFLAGS) \
	$(CONFIG_CPPFLAGS)

KERN_CFLAGS := \
	-m68040 \
	-msoft-float \
	-ffreestanding \
	-fno-pic \
	-fno-pie \
	-fno-common \
	-fno-strict-aliasing \
	-fno-stack-protector \
	-fno-asynchronous-unwind-tables \
	-fno-unwind-tables \
	-ffunction-sections \
	-fdata-sections \
	-Os \
	-Wall \
	-Wextra \
	-Werror \
	-Wframe-larger-than=8192 \
	-fno-builtin

# The compiler's helpers, from the multilib of the 68040 without a floating-point unit.
MAC68K_LIBGCC := $(GCC_INSTALL)/lib/gcc/$(GCC_TARGET)/$(ZEDBSD_GCC_VERSION)/m68040/softfp/libgcc.a

# HAL C Sources
KERN_SOURCES += \
	src/hal/m68k/m68040/cpu.c \
	src/hal/m68k/m68040/int.c \
	src/hal/m68k/m68040/space.c \
	src/hal/m68k/m68040/task.c \
	src/hal/m68k/m68040/page.c \
	src/hal/m68k/m68040/lib.c \
	src/hal/m68k/m68040/debug.c \
	src/hal/m68k/atomic.c \
	src/hal/idle-suspend-unsupported.c \
	src/hal/m68k/bsp-mac68k/board.c \
	src/hal/m68k/bsp-mac68k/bootinfo.c \
	src/hal/m68k/bsp-mac68k/via.c \
	src/hal/m68k/bsp-mac68k/clock.c \
	src/hal/m68k/bsp-mac68k/framebuffer.c \
	src/hal/m68k/bsp-mac68k/cons.c \
	src/hal/m68k/bsp-mac68k/font.c

# HAL Assembly Sources
KERN_ASM += \
	src/hal/m68k/bsp-mac68k/start.S \
	src/hal/m68k/m68040/entry.S

# Kernel Sources
KERN_SOURCES += \
	src/kern/platform/mac68k.c

VMUNIX_OBJS := \
	$(patsubst %.S,$(BUILD)/%.o,$(KERN_ASM)) \
	$(patsubst %.c,$(BUILD)/%.o,$(KERN_SOURCES))

.PHONY: vmunix-platform
vmunix-platform: $(BUILD)/vmunix

# The kernel is an ELF the loader places at its physical addresses.
$(BUILD)/vmunix: $(VMUNIX_OBJS) mk/platform-$(PLATFORM)/vmunix.ld
	@echo "  LD      vmunix"
	@$(LD) --gc-sections -z max-page-size=4096 -z noexecstack -T mk/platform-$(PLATFORM)/vmunix.ld -nostdlib -Map $@.map $(VMUNIX_OBJS) $(MAC68K_LIBGCC) -o $@
	@test -z "$$($(NM) -u $@ | awk '$$1 == "U"')" || { $(NM) -u $@; exit 1; }

$(BUILD)/%.o: %.c
	@echo "  CC      $<"
	@mkdir -p $(dir $@)
	@$(CC) $(KERN_CPPFLAGS) $(KERN_CFLAGS) -c $< -o $@

$(BUILD)/%.o: %.S
	@echo "  AS      $<"
	@mkdir -p $(dir $@)
	@$(CC) $(KERN_CPPFLAGS) $(KERN_CFLAGS) -c $< -o $@

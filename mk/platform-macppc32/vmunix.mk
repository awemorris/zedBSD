# zedBSD
# Copyright (C) 2026 Awe Morris
#
# SPDX-License-Identifier: Zlib

KERN_CPPFLAGS := \
	-nostdlib \
	-nostdinc \
	-I$(BUILD)/sysroot/usr/include \
	-Iinclude \
	-Isrc \
	-I. \
	-D__ZEDBSD__ \
	-DHAL_ARCH_PPC32 \
	-DHAL_BOARD_MAC99 \
	-DKERN_USER_ABI_PPC32 \
	-DKERN_UAPI_NATIVE \
	-DKERN_ATOMIC_SERIALIZED \
	-DZEDBSD_VERSION=\"$(VERSION)\" \
	$(KERN_CPPFLAGS) \
	$(CONFIG_CPPFLAGS)

KERN_CFLAGS := \
	--target=powerpc-unknown-zedbsd \
	-mcpu=ppc \
	-mno-red-zone \
	-msoft-float \
	-ffreestanding \
	-fno-pic \
	-fno-pie \
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
	-Wno-atomic-alignment \
	-fno-builtin

# HAL C Sources
KERN_SOURCES += \
	src/hal/ppc32/cpu.c \
	src/hal/ppc32/int.c \
	src/hal/ppc32/space.c \
	src/hal/ppc32/task.c \
	src/hal/ppc32/page.c \
	src/hal/ppc32/lib.c \
	src/hal/ppc32/debug.c \
	src/hal/ppc32/atomic.c \
	src/hal/idle-suspend-unsupported.c \
	src/hal/ppc32/bsp-mac99/board.c \
	src/hal/ppc32/bsp-mac99/firmware.c \
	src/hal/ppc32/bsp-mac99/irq.c \
	src/hal/ppc32/bsp-mac99/clock.c \
	src/hal/ppc32/bsp-mac99/pmu.c \
	src/hal/ppc32/bsp-mac99/framebuffer.c \
	src/hal/ppc32/bsp-mac99/cons.c \
	src/hal/ppc32/bsp-mac99/font.c

# HAL Assembly Sources
KERN_ASM += \
	src/hal/ppc32/entry.S

# Kernel Sources
KERN_SOURCES += \
	src/kern/platform/mac99.c \
	src/drivers/platform/mac99/mac99-pci.c \
	src/drivers/platform/mac99/mac99-ata.c \
	src/drivers/platform/mac99/mac99-pmu.c \
	src/drivers/platform/mac99/mac99-ethernet.c \
	src/drivers/platform/mac99/mac99-i2c.c \
	src/drivers/platform/mac99/mac99-adt746x.c \
	src/drivers/platform/mac99/mac99-adm1030.c \
	src/drivers/platform/mac99/mac99-fan.c \
	src/drivers/ethernet/sungem.c \
	src/drivers/ethernet/mii-phy.c \
	src/drivers/generic/ata-pio.c \
	src/drivers/generic/serial-console.c \
	src/drivers/disklabel/apm.c \
	src/drivers/disklabel/gpt.c \
	src/drivers/disklabel/pcat.c \
	src/drivers/disklabel/mbr.c

VMUNIX_OBJS := \
	$(patsubst %.c,$(BUILD)/%.o,$(KERN_SOURCES)) \
	$(patsubst %.S,$(BUILD)/%.o,$(KERN_ASM))

.PHONY: vmunix-platform
vmunix-platform: $(BUILD)/vmunix

$(BUILD)/vmunix: $(VMUNIX_OBJS) $(BUILD)/libc/libclang_rt.builtins.a
	@echo "  LD      vmunix"
	@$(LD) -m elf32ppc --gc-sections -z max-page-size=4096 -T mk/platform-$(PLATFORM)/vmunix.ld -nostdlib -Map $@.map $(VMUNIX_OBJS) $(BUILD)/libc/libclang_rt.builtins.a -o $@
	@test -z "$$($(NM) -u $@ | awk '$$1 == "U"')" || { $(NM) -u $@; exit 1; }

$(BUILD)/%.o: %.c
	@echo "  CC      $<"
	@mkdir -p $(dir $@)
	@$(CC) $(KERN_CPPFLAGS) $(KERN_CFLAGS) -c $< -o $@

$(BUILD)/%.o: %.S
	@echo "  AS      $<"
	@mkdir -p $(dir $@)
	@$(CC) $(KERN_CPPFLAGS) $(KERN_CFLAGS) -c $< -o $@

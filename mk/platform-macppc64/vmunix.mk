# zedBSD
# Copyright (C) 2026 Awe Morris
#
# SPDX-License-Identifier: Zlib

#
# PowerPC Mac (64-bit) kernel rules
#

# The compiler's target: big-endian 64-bit PowerPC with the ELFv2 ABI.
MACPPC64_TARGET_FLAGS := --target=powerpc64-unknown-zedbsd

KERN_CPPFLAGS := \
	-nostdlib \
	-nostdinc \
	-I$(BUILD)/sysroot/usr/include \
	-Iinclude \
	-Isrc \
	-I. \
	-D__ZEDBSD__ \
	-DHAL_ARCH_PPC64 \
	-DHAL_BOARD_MACPPC64 \
	-DKERN_USER_ABI_PPC64 \
	-DKERN_UAPI_NATIVE \
	-DKERN_ATOMIC_SERIALIZED \
	-DZEDBSD_VERSION=\"$(VERSION)\" \
	$(KERN_CPPFLAGS) \
	$(CONFIG_CPPFLAGS)

# The kernel's C uses no floating point and no vector unit.
KERN_CFLAGS := \
	$(MACPPC64_TARGET_FLAGS) \
	-mcpu=970 \
	-msoft-float \
	-mno-altivec \
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
#
# The PowerPC Mac's firmware reading, tick, interrupt controller, power
# manager and console are shared with the 32-bit PowerPC Mac
# (src/hal/powermac/).
KERN_SOURCES += \
	src/hal/ppc64/cpu.c \
	src/hal/ppc64/int.c \
	src/hal/ppc64/space.c \
	src/hal/ppc64/task.c \
	src/hal/ppc64/page.c \
	src/hal/ppc64/lib.c \
	src/hal/ppc64/debug.c \
	src/hal/ppc64/atomic.c \
	src/hal/idle-suspend-unsupported.c \
	src/hal/ppc64/bsp-macppc64/board.c \
	src/hal/powermac/firmware.c \
	src/hal/powermac/irq.c \
	src/hal/powermac/clock.c \
	src/hal/powermac/pmu.c \
	src/hal/powermac/framebuffer.c \
	src/hal/powermac/cons.c \
	src/hal/powermac/font.c

# HAL Assembly Sources
KERN_ASM += \
	src/hal/ppc64/entry.S

# Kernel Sources
#
# The PowerPC Mac's binding: the console on the firmware's framebuffer,
# the PCI host bridge, the I/O controller's ATA channels, the power
# manager's keyboard, the built-in Ethernet and the disk labels.
KERN_SOURCES += \
	src/kern/platform/mac99.c \
	src/drivers/platform/mac99/mac99-pci.c \
	src/drivers/platform/mac99/mac99-ata.c \
	src/drivers/platform/mac99/mac99-pmu.c \
	src/drivers/platform/mac99/mac99-ethernet.c \
	src/drivers/platform/mac99/mac99-i2c.c \
	src/drivers/platform/mac99/mac99-fan.c \
	src/drivers/platform/mac99/mac99-adt746x.c \
	src/drivers/platform/mac99/mac99-adm1030.c \
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

# The objects compile against the sysroot's headers.
$(VMUNIX_OBJS): | $(HEADERS_INSTALLED_STAMP)

.PHONY: vmunix-platform
vmunix-platform: $(BUILD)/vmunix $(BUILD)/vmunix.elf32

$(BUILD)/vmunix: $(VMUNIX_OBJS) $(BUILD)/libc/libclang_rt.builtins.a mk/platform-macppc64/vmunix.ld
	@echo "  LD      vmunix"
	@$(LD) -m elf64ppc --gc-sections -z max-page-size=4096 -T mk/platform-$(PLATFORM)/vmunix.ld -nostdlib -Map $@.map $(VMUNIX_OBJS) $(BUILD)/libc/libclang_rt.builtins.a -o $@
	@test -z "$$($(NM) -u $@ | awk '$$1 == "U"')" || { $(NM) -u $@; exit 1; }

# Open Firmware is handed a 32-bit ELF container of the same image: the
# segments and the entry are the kernel's, which starts in 32-bit mode.
$(BUILD)/vmunix.elf32: $(BUILD)/vmunix
	@echo "  OBJCOPY vmunix.elf32"
	@$(OBJCOPY) -O elf32-powerpc $< $@

$(BUILD)/%.o: %.c
	@echo "  CC      $<"
	@mkdir -p $(dir $@)
	@$(CC) $(KERN_CPPFLAGS) $(KERN_CFLAGS) -c $< -o $@

$(BUILD)/%.o: %.S
	@echo "  AS      $<"
	@mkdir -p $(dir $@)
	@$(CC) $(KERN_CPPFLAGS) $(KERN_CFLAGS) -c $< -o $@

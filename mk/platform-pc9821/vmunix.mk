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
	-DHAL_ARCH_I386 \
	-DHAL_BOARD_PC98 \
	-DKERN_USER_ABI_ILP32 \
	-DKERN_UAPI_NATIVE \
	-DKERN_ATOMIC_SERIALIZED \
	-DZEDBSD_VERSION=\"$(VERSION)\" \
	$(KERN_CPPFLAGS) \
	$(CONFIG_CPPFLAGS)

KERN_CFLAGS := \
	--target=i386-unknown-zedbsd \
	-m32 \
	-march=i386 \
	-mno-red-zone \
	-mgeneral-regs-only \
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
	src/hal/x86/rtc.c \
	src/hal/x86/boot-parameters.c \
	src/hal/x86/io.c \
	src/hal/i386/smp.c \
	src/hal/i386/percpu.c \
	src/hal/i386/lib.c \
	src/hal/i386/atomic.c \
	src/hal/i386/irq.c \
	src/hal/i386/mps.c \
	src/hal/i386/acpi.c \
	src/hal/i386/lapic.c \
	src/hal/i386/ioapic.c \
	src/hal/i386/page.c \
	src/hal/i386/space.c \
	src/hal/i386/int.c \
	src/hal/i386/cmain.c \
	src/hal/i386/task.c \
	src/hal/i386/debug.c \
	src/hal/i386/bsp-pc98/boot.c \
	src/hal/i386/bsp-pc98/cons.c \
	src/hal/i386/bsp-pc98/pic.c \
	src/hal/i386/bsp-pc98/pit.c \
	src/hal/idle-suspend-unsupported.c

# HAL Assembly Sources
KERN_ASM += \
	src/hal/i386/bsp-pc98/boot-header.S \
	src/hal/i386/locore.S \
	src/hal/i386/ap-trampoline.S \
	src/hal/i386/trap.S \
	src/hal/i386/dispatch.S

# Kernel Sources
KERN_SOURCES += \
	src/kern/platform/pc98.c \
	src/drivers/platform/pc98/pc98-ide.c \
	src/drivers/platform/pc98/pc98-busmouse.c \
	src/drivers/platform/pc98/pc98-keyboard.c \
	src/drivers/disklabel/mbr.c \
	src/drivers/disklabel/pc98.c \
	src/drivers/ethernet/dp8390.c

VMUNIX_OBJS := \
	$(patsubst %.S,$(BUILD)/%.o,$(KERN_ASM)) \
	$(patsubst %.c,$(BUILD)/%.o,$(KERN_SOURCES))

.PHONY: vmunix-platform
vmunix-platform: $(BUILD)/vmunix

$(BUILD)/vmunix: $(VMUNIX_OBJS) $(BUILD)/libc/libclang_rt.builtins.a
	@echo "  LD      vmunix"
	@$(LD) -m elf_i386 --gc-sections -z max-page-size=512 -T mk/platform-$(PLATFORM)/vmunix.ld -nostdlib -Map $@.map $(VMUNIX_OBJS) $(BUILD)/libc/libclang_rt.builtins.a -o $@
	@test -z "$$($(NM) -u $@ | awk '$$1 == "U"')" || { $(NM) -u $@; exit 1; }
	@$(PYTHON) mk/platform-pc9821/tools/patch-stage2.py $@

$(BUILD)/%.o: %.c
	@echo "  CC      $<"
	@mkdir -p $(dir $@)
	@$(CC) $(KERN_CPPFLAGS) $(KERN_CFLAGS) -c $< -o $@

$(BUILD)/%.o: %.S
	@echo "  AS      $<"
	@mkdir -p $(dir $@)
	@$(CC) $(KERN_CPPFLAGS) $(KERN_CFLAGS) -c $< -o $@

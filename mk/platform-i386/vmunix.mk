# zedBSD
# Copyright (C) 2026 Awe Morris
#
# SPDX-License-Identifier: Zlib

KERN_CPPFLAGS := \
	-nostdlib \
	-nostdinc \
	-Ibuild/i386/sysroot/usr/include \
	-Iinclude \
	-Isrc \
	-I. \
	-D__ZEDBSD__ \
	-DHAL_ARCH_I386 \
	-DHAL_BOARD_PCAT \
	-DHAL_PCAT_DEBUGCON \
	-DKERN_USER_ABI_ILP32 \
	-DKERN_UAPI_NATIVE \
	-DKERN_ATOMIC_SERIALIZED \
	-DZEDBSD_VERSION=\"$(VERSION)\" \
	$(KERN_CPPFLAGS) \
	$(CONFIG_CPPFLAGS)

KERN_CFLAGS := \
	--target=i386-unknown-zedbsd \
	-m32 \
	-march=i686 \
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
	src/hal/i386/bsp-pcat/boot.c \
	src/hal/i386/bsp-pcat/cons.c \
	src/hal/i386/bsp-pcat/pic.c \
	src/hal/i386/bsp-pcat/pit.c \
	src/hal/idle-suspend-unsupported.c

# HAL Assembly Sources
KERN_ASM += \
	src/hal/i386/locore.S \
	src/hal/i386/ap-trampoline.S \
	src/hal/i386/trap.S \
	src/hal/i386/dispatch.S

# Kernel Sources
KERN_SOURCES += \
	src/kern/platform/pcat.c \
	src/drivers/platform/pcat/pcat-ide.c \
	src/drivers/platform/pcat/cpu-x86/cpux86.c \
	src/drivers/platform/pcat/cpu-x86/cpux86-cpuid.c \
	src/drivers/platform/pcat/cpu-x86/cpu-x86.c \
	src/drivers/platform/pcat/cpu-x86/cpu-x86-insn.c \
	src/drivers/platform/pcat/serial-mirror.c \
	src/drivers/disklabel/mbr.c \
	src/drivers/disklabel/gpt.c \
	src/drivers/disklabel/pcat.c \
	src/drivers/pci/pci-pcat.c \
	src/drivers/pci/pci-pcat-assign.c \
	src/drivers/pci/pci-window.c \
	src/drivers/isa/ps2-8042.c \
	src/drivers/isa/ne2000.c \
	src/drivers/ethernet/dp8390.c

VMUNIX_OBJS := \
	$(patsubst %.c,$(BUILD)/%.o,$(KERN_SOURCES)) \
	$(patsubst %.S,$(BUILD)/%.o,$(KERN_ASM))

.PHONY: vmunix-platform
vmunix-platform: $(BUILD)/vmunix

$(BUILD)/vmunix: $(VMUNIX_OBJS) $(BUILD)/libc/libclang_rt.builtins.a
	@echo "  LD      vmunix"
	@$(LD) -m elf_i386 --gc-sections -z max-page-size=4096 -T mk/platform-$(PLATFORM)/vmunix.ld -nostdlib -Map $@.map $(VMUNIX_OBJS) $(BUILD)/libc/libclang_rt.builtins.a -o $@
	@test -z "$$($(NM) -u $@ | awk '$$1 == "U"')" || { $(NM) -u $@; exit 1; }

$(BUILD)/%.o: %.c
	@echo "  CC      $<"
	@mkdir -p $(dir $@)
	@$(CC) $(KERN_CPPFLAGS) $(KERN_CFLAGS) -c $< -o $@

$(BUILD)/%.o: %.S
	@echo "  AS      $<"
	@mkdir -p $(dir $@)
	@$(CC) $(KERN_CPPFLAGS) $(KERN_CFLAGS) -c $< -o $@

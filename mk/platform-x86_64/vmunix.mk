#
# zedBSD amd64/PC-AT bootstrap rules.
# Copyright (C) 2026 Awe Morris
#
# SPDX-License-Identifier: Zlib
#

KERN_CPPFLAGS := \
	-nostdlib \
	-nostdinc \
	-Ibuild/x86_64/sysroot/usr/include \
	-Iinclude \
	-Isrc \
	-I. \
	-D__ZEDBSD__ \
	-DHAL_ARCH_X86_64 \
	-DHAL_BOARD_PCAT \
	-DHAL_PCAT_DEBUGCON \
	-DKERN_USER_ABI_LP64 \
	-DKERN_UAPI_NATIVE \
	-DZEDBSD_VERSION=\"$(VERSION)\" \
	-DPCAT_VGA_APERTURE_ADDRESS=0xffffffffc1400000ULL \
	-DPCAT_CIRRUS_APERTURE_ADDRESS=0xffffffffc0000000ULL \
	$(KERN_CPPFLAGS) \
	$(CONFIG_CPPFLAGS)

KERN_CFLAGS := \
	-m64 \
	-mcmodel=kernel \
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
	-fno-builtin

# HAL C Sources
KERN_SOURCES += \
	src/hal/x86/rtc.c \
	src/hal/x86/boot-parameters.c \
	src/hal/x86/io.c \
	src/hal/x86_64/asm.c \
	src/hal/x86_64/lib.c \
	src/hal/x86_64/page.c \
	src/hal/x86_64/pmem-range.c \
	src/hal/x86_64/ram-map.c \
	src/hal/x86_64/framebuffer-map.c \
	src/hal/x86_64/space.c \
	src/hal/x86_64/image.c \
	src/hal/x86_64/acpi-window.c \
	src/hal/x86_64/cmain.c \
	src/hal/x86_64/descriptor.c \
	src/hal/x86_64/int.c \
	src/hal/x86_64/irq.c \
	src/hal/x86_64/msi-source.c \
	src/hal/x86_64/task.c \
	src/hal/x86_64/percpu.c \
	src/hal/x86_64/smp.c \
	src/hal/x86_64/bsp-pcat/boot.c \
	src/hal/x86_64/bsp-pcat/handoff-validation.c \
	src/hal/x86_64/bsp-pcat/cons.c \
	src/hal/x86_64/bsp-pcat/pic.c \
	src/hal/x86_64/bsp-pcat/clock.c \
	src/hal/x86_64/bsp-pcat/acpi.c \
	src/hal/x86_64/bsp-pcat/lapic.c \
	src/hal/x86_64/bsp-pcat/idle-suspend.c \
	src/hal/x86_64/bsp-pcat/early-init-policy.c \
	src/hal/x86_64/bsp-pcat/timecounter-policy.c \
	src/hal/x86_64/bsp-pcat/timecounter.c \
	src/hal/x86_64/bsp-pcat/mcfg.c \
	src/hal/x86_64/bsp-pcat/ioapic.c

# HAL Assembly Sources
KERN_ASM += \
	src/hal/x86_64/locore.S \
	src/hal/x86_64/trap.S \
	src/hal/x86_64/dispatch.S \
	src/hal/x86_64/ap-trampoline.S

# Kernel Sources
KERN_SOURCES += \
	src/kern/platform/pcat.c \
	src/drivers/platform/pcat/uefi-variables.c \
	src/drivers/platform/pcat/pcat-ide.c \
	src/drivers/platform/pcat/cpu-x86/cpu-x86.c \
	src/drivers/platform/pcat/cpu-x86/cpu-x86-insn.c \
	src/drivers/platform/pcat/cpu-x86/cpu-x86-control.c \
	src/drivers/platform/pcat/cpu-x86/cpux86.c \
	src/drivers/platform/pcat/cpu-x86/cpux86-cpuid.c \
	src/drivers/platform/pcat/cpu-x86/cpux86-hwp.c \
	src/drivers/platform/pcat/cpu-x86/cpux86-power.c \
	src/drivers/platform/pcat/cpu-x86/cpux86-pstate.c \
	src/drivers/disklabel/mbr.c \
	src/drivers/disklabel/gpt.c \
	src/drivers/disklabel/pcat.c \
	src/drivers/pci/pci-pcat.c \
	src/drivers/pci/pci-pcat-assign.c \
	src/drivers/pci/pci-window.c \
	src/drivers/isa/ps2-8042.c \
	src/drivers/platform/pcat/serial-mirror.c

ifneq ($(filter y,$(CONFIG_DRIVER_ACPI_SDHCI)),)
KERN_CPPFLAGS += -DCONFIG_DRIVER_ACPI_SDHCI=1
KERN_SOURCES += \
	src/drivers/mmc/sdhci.c \
	src/drivers/mmc/sdhci-acpi.c
endif

VMUNIX_OBJS := \
	$(patsubst %.c,$(BUILD)/%.o,$(KERN_SOURCES)) \
	$(patsubst %.S,$(BUILD)/%.o,$(KERN_ASM))

.PHONY: vmunix-platform
vmunix-platform: $(BUILD)/vmunix

$(BUILD)/vmunix: $(VMUNIX_OBJS)
	@echo "  LD      vmunix"
	@$(LD) -m elf_x86_64 --gc-sections -z max-page-size=4096 -T mk/platform-$(PLATFORM)/vmunix.ld -nostdlib -Map $@.map $(VMUNIX_OBJS) -o $@
	@test -z "$$($(NM) -u $@ | awk '$$1 == "U"')" || { $(NM) -u $@; exit 1; }

$(BUILD)/%.o: %.c
	@echo "  CC      $<"
	@mkdir -p $(dir $@)
	@$(CC) $(KERN_CPPFLAGS) $(KERN_CFLAGS) -c $< -o $@

$(BUILD)/%.o: %.S
	@echo "  AS      $<"
	@mkdir -p $(dir $@)
	@$(CC) $(KERN_CPPFLAGS) $(KERN_CFLAGS) -c $< -o $@

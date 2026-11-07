# zedBSD
# Copyright (C) 2026 Awe Morris
#
# SPDX-License-Identifier: Zlib

PLATFORM_DIR := $(patsubst %/,%,$(dir $(lastword $(MAKEFILE_LIST))))
REPO_ROOT    ?= $(abspath $(PLATFORM_RPI5_DIR)/../..)

KERN_CPPFLAGS := \
	-nostdlibinc \
	-Iinclude \
	-Isrc \
	-I. \
	-Isrc/hal/riscv64 \
	-DHAL_ARCH_RISCV64 \
	-DHAL_BSP_VISIONFIVE2 \
	-DKERN_USER_ABI_RISCV64 \
	-DKERN_USER_ABI_LP64

KERN_CFLAGS := \
	-march=rv64imac_zicsr_zifencei \
	-mabi=lp64 -mcmodel=medany -mno-relax \
	-ffreestanding \
	-fno-pic \
	-fno-pie \
	-fno-stack-protector \
	-fno-asynchronous-unwind-tables \
	-fno-unwind-tables \
	-fno-common \
	-ffunction-sections \
	-fdata-sections \
	-Os \
	-Wall \
	-Wextra \
	-Werror

# HAL C Sources
KENR_SOURCES += \
	src/hal/riscv64/cmain.c \
	src/hal/riscv64/bootinfo.c \
	src/hal/riscv64/fdt.c \
	src/hal/riscv64/sbi.c \
	src/hal/riscv64/percpu.c \
	src/hal/riscv64/smp.c \
	src/hal/riscv64/int.c \
	src/hal/riscv64/irq.c \
	src/hal/riscv64/plic.c \
	src/hal/riscv64/clock.c \
	src/hal/riscv64/page.c \
	src/hal/riscv64/space.c \
	src/hal/riscv64/window.c \
	src/hal/riscv64/task.c \
	src/hal/riscv64/debug.c \
	src/hal/riscv64/asm.c \
	src/hal/riscv64/lib.c \
	src/hal/riscv64/loader.c \
	src/hal/riscv64/cons.c \
	src/hal/riscv64/fbcons.c \
	src/hal/riscv64/font.c \
	src/hal/riscv64/bsp-visionfive2/board.c \
	src/hal/idle-suspend-unsupported.c

# HAL Assembly Sources
KENR_ASM += \
	src/hal/riscv64/locore.S \
	src/hal/riscv64/trap.S \
	src/hal/riscv64/dispatch.S

# Kernel Sources for Vision Five 2
KERN_SOURCES += \
	src/kern/platform/visionfive2.c \
	src/drivers/platform/visionfive2/vf2-dmac.c \
	src/drivers/platform/visionfive2/vf2-pwmdac.c \
	src/drivers/platform/visionfive2/vf2-syscon.c \
	src/drivers/platform/visionfive2/vf2-crg.c \
	src/drivers/platform/visionfive2/vf2-pinctrl.c \
	src/drivers/platform/visionfive2/vf2-mmc.c \
	src/drivers/platform/visionfive2/vf2-pcie.c \
	src/drivers/pci/pci.c \
	src/drivers/pci/pci-plda.c \
	src/drivers/pci/pci-ecam-fdt.c \
	src/drivers/disklabel/gpt.c \
	src/drivers/disklabel/pcat.c \
	src/drivers/generic/serial-console.c \
	src/drivers/dma/dw-axi-dmac.c \
	src/drivers/mmc/dw-mmc.c \
	src/drivers/serial/ns16550.c

VMUNIX_OBJS := \
	$(sort $(patsubst %.c,$(BUILD)/%.o,$(KERN_SOURCES))) \
	$(sort $(patsubst %.S,$(BUILD)/%.o,$(KERN_ASM)))

$(BUILD)/kernel.elf: $(VMUNIX_OBJS) mk/platform-visionfive2/vmunix.ld mk/platform-visionfive2/tools/check-riscv64vmunix.py
	@mkdir -p $(dir $@)
	$(LD) -m elf64lriscv --gc-sections --no-relax -z max-page-size=4096 -T $(RISCV64_PLATFORM)/vmunix.ld -nostdlib $(RISCV64_VMUNIX_OBJS) -o $@
	@test -z "$$($(NM) -u $@ | $(AWK) '$$1 == "U"')" || { $(NM) -u $@; exit 1; }
	$(PYTHON) mk/platform-visionfive2/tools/check-riscv64-vmunix.py --elf $@

$(BUILD)/vmunix: $(BUILD)/kernel.elf mk/platform-visionfive2/tools/check-riscv64-vmunix.py
	@mkdir -p $(dir $@)
	$(OBJCOPY) -O binary $< $@
	$(PYTHON) mk/platform-visionfive2//tools/check-riscv64-vmunix.py --elf $< --image $@

# Build rule for C sources
$(BUILD)/%.o: %.c
	@mkdir -p $(dir $@)
	$(CC) $(KERN_CPPFLAGS) $(KERN_CFLAGS) -c $< -o $@

# Build rule for Assembly sources
$(BUILD)/%.o: %.S
	@mkdir -p $(dir $@)
	$(CC) $(KERN_CPPFLAGS) $(KERN_CFLAGS) -c $< -o $@

.PHONY: vmunix-platform
vmunix-platform: $(BUILD)/vmunix

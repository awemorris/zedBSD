#
# zedBSD rpi5 (ARM64) bootstrap rules.
# Copyright (C) 2026 Awe Morris
#
# SPDX-License-Identifier: Zlib
#

PLATFORM_RPI5_DIR := $(patsubst %/,%,$(dir $(lastword $(MAKEFILE_LIST))))
REPO_ROOT         ?= $(abspath $(PLATFORM_RPI5_DIR)/../..)

KERN_CPPFLAGS := \
	-nostdlib \
	-nostdinc \
	-I$(BUILD)/sysroot/usr/include \
	-Iinclude \
	-Isrc \
	-I. \
	-Isrc/hal/arm64 \
	-Isrc/hal/arm64/bsp-rpi5 \
	-Isrc/hal/arm64/bsp-rpi-common \
	-D__ZEDBSD__ \
	-DHAL_ARCH_ARM64 \
	-DHAL_BOARD_RPI5 \
	-DKERN_USER_ABI_AARCH64 \
	-DKERN_USER_ABI_LP64 \
	-DKERN_UAPI_NATIVE \
	-DZEDBSD_VERSION=\"$(VERSION)\" \
	$(KERN_CPPFLAGS) \
	$(CONFIG_CPPFLAGS)

KERN_CFLAGS := \
	--target=aarch64-unknown-zedbsd \
	-march=armv8-a \
	-mno-outline-atomics \
	-mgeneral-regs-only \
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
	-Werror \
	-fno-builtin

# HAL C Sources
KERN_SOURCES += \
	src/hal/arm64/smp.c \
	src/hal/arm64/percpu.c \
	src/hal/arm64/watch.c \
	src/hal/idle-suspend-unsupported.c \
	src/hal/arm64/asm.c \
	src/hal/arm64/lib.c \
	src/hal/arm64/page.c \
	src/hal/arm64/space.c \
	src/hal/arm64/int.c \
	src/hal/arm64/irq.c \
	src/hal/arm64/task.c \
	src/hal/arm64/cmain.c \
	src/hal/arm64/debug.c \
	src/hal/arm64/cons.c \
	src/hal/arm64/framebuffer.c \
	src/hal/arm64/boot.c \
	src/hal/arm64/gic.c \
	src/hal/arm64/clock.c \
	src/hal/arm64/font.c \
	src/hal/arm64/led.c \
	src/hal/arm64/bsp-rpi5/uart.c \
	src/hal/arm64/bsp-rpi5/fdt.c \
	src/hal/arm64/bsp-rpi5/activity-led.c \
	src/hal/arm64/bsp-rpi5/board.c \
	src/hal/arm64/bsp-rpi-common/mailbox.c \
	src/hal/arm64/bsp-rpi-common/framebuffer-mailbox.c

# HAL Assembly Sources
KERN_ASM += \
	src/hal/arm64/locore.S \
	src/hal/arm64/trap.S \
	src/hal/arm64/dispatch.S

# Kernel Sources for RPi5
KERN_SOURCES += \
	src/kern/platform/rpi5.c \
	src/drivers/disklabel/mbr.c \
	src/drivers/platform/rpi-common/rpi-sdhci.c \
	src/drivers/platform/rpi-common/rpi-console.c \
	src/drivers/platform/rpi-common/rpi-mmc-irq.c \
	src/drivers/platform/rpi-common/rpi-sdio-host.c \
	src/drivers/platform/rpi-common/rpi-sdio.c \
	src/drivers/platform/rpi-common/rpi-gpio.c \
	src/drivers/platform/rpi5/rp1.c \
	src/drivers/platform/rpi5/rp1-gpio.c \
	src/drivers/platform/rpi5/rp1-irq.c \
	src/drivers/platform/rpi5/rp1-usb.c \
	src/drivers/platform/rpi5/rp1-ethernet.c \
	src/drivers/platform/rpi5/rpi5-gio.c \
	src/drivers/platform/rpi5/rpi5-pinctrl.c \
	src/drivers/platform/rpi5/rpi5-wifi.c \
	src/drivers/platform/rpi5/rpi5-serror.c \
	src/drivers/platform/rpi5/rpi5-pcie.c \
	src/drivers/platform/rpi5/rpi5-mip.c \
	src/drivers/wifi/bcm43455/bcm43455.c \
	src/drivers/wifi/bcm43455/bcm43455-bcdc.c \
	src/drivers/wifi/bcm43455/bcm43455-bus.c \
	src/drivers/wifi/bcm43455/bcm43455-chip.c \
	src/drivers/wifi/bcm43455/bcm43455-event.c \
	src/drivers/wifi/bcm43455/bcm43455-files.c \
	src/drivers/wifi/bcm43455/bcm43455-nvram.c \
	src/drivers/wifi/bcm43455/bcm43455-radio.c \
	src/drivers/wifi/bcm43455/bcm43455-sdpcm.c \
	src/drivers/wifi/bcm43455/bcm43455-station.c \
	src/drivers/mmc/sdhci.c \
	src/drivers/generic/fdt.c \
	src/drivers/generic/dma.c \
	src/drivers/pci/pci-brcmstb.c

VMUNIX_OBJS := \
	$(sort $(patsubst %.c,$(BUILD)/%.o,$(KERN_SOURCES))) \
	$(sort $(patsubst %.S,$(BUILD)/%.o,$(KERN_ASM)))

.PHONY: vmunix-platform
vmunix-platform: $(BUILD)/vmunix

$(BUILD)/kernel.elf: $(VMUNIX_OBJS) mk/platform-rpi5/vmunix.ld mk/platform-rpi5/tools/check-arm64-vmunix.py
	@echo "  LD      kernel.elf"
	@mkdir -p $(dir $@)
	@$(LD) -m aarch64elf --gc-sections -z max-page-size=4096 -T mk/platform-rpi5/vmunix.ld -nostdlib -Map $@.map $(VMUNIX_OBJS) -o $@
	@test -z "$$($(NM) -u $@ | awk '$$1 == "U"')" || { $(NM) -u $@; exit 1; }
	@$(PYTHON) mk/platform-rpi5/tools/check-arm64-vmunix.py --elf $@

$(BUILD)/vmunix: $(BUILD)/kernel.elf mk/platform-rpi5/tools/check-arm64-vmunix.py
	@echo "  OBJCOPY kernel.elf vmunix"
	@mkdir -p $(dir $@)
	@$(OBJCOPY) -O binary $< $@
	@$(PYTHON) mk/platform-rpi5/tools/check-arm64-vmunix.py --elf $< --image $@

$(BUILD)/%.o: %.c
	@echo "  CC      $<"
	@mkdir -p $(dir $@)
	@$(CC) $(KERN_CPPFLAGS) $(KERN_CFLAGS) -c $< -o $@

$(BUILD)/%.o: %.S
	@echo "  AS      $<"
	@mkdir -p $(dir $@)
	@$(CC) $(KERN_CPPFLAGS) $(KERN_CFLAGS) -c $< -o $@

# zedBSD arm64/Raspberry Pi 4 bootstrap rules.
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib

ARM64_CC ?= aarch64-linux-gnu-gcc
ARM64_LD ?= aarch64-linux-gnu-ld
ARM64_OBJCOPY ?= aarch64-linux-gnu-objcopy
ARM64_NM ?= aarch64-linux-gnu-nm
ARM64_PLATFORM := platform/arm64

ARM64_CPPFLAGS := -nostdinc -Iinclude -Iinclude/uapi -Isrc -I. \
	-Ilibc/include -Isrc/hal/arm64 -DHAL_ARCH_ARM64 -DHAL_BOARD_RPI4 \
	-DZEDBSD_USER_ABI_AARCH64 -DZEDBSD_USER_ABI_LP64
ARM64_CFLAGS := -march=armv8-a -mgeneral-regs-only -ffreestanding \
	-fno-pic -fno-pie -fno-stack-protector -fno-asynchronous-unwind-tables \
	-fno-unwind-tables -fno-common -Os -Wall -Wextra -Werror

ARM64_BOOT_C := src/hal/arm64/asm.c src/hal/arm64/lib.c \
	src/hal/arm64/page.c src/hal/arm64/space.c \
	src/hal/arm64/int.c src/hal/arm64/irq.c \
	src/hal/arm64/task.c \
	src/hal/arm64/fb.c \
	src/hal/arm64/cmain.c src/hal/arm64/bsp-rpi4/uart.c \
	src/hal/arm64/bsp-rpi4/cons.c src/hal/arm64/bsp-rpi4/fdt.c \
	src/hal/arm64/bsp-rpi4/mailbox.c src/hal/arm64/bsp-rpi4/framebuffer.c \
	src/hal/arm64/bsp-rpi4/boot.c src/hal/arm64/bsp-rpi4/gic.c \
	src/hal/arm64/bsp-rpi4/clock.c
ARM64_BOOT_S := src/hal/arm64/locore.S src/hal/arm64/trap.S \
	src/hal/arm64/dispatch.S
ARM64_BOOT_OBJS := $(patsubst %.c,$(BUILD)/%.o,$(ARM64_BOOT_C)) \
	$(patsubst %.S,$(BUILD)/%.o,$(ARM64_BOOT_S))

ARM64_KERNEL_SOURCES := \
	src/kern/main.c src/kern/env.c src/kern/fs.c src/kern/namespace.c \
	src/kern/fat.c src/kern/fat-lfn.c src/kern/fat16.c src/kern/fat-vfs.c \
	src/kern/inode.c src/kern/file.c src/kern/namecache.c src/kern/namei.c \
	src/kern/mount.c src/kern/rootfs.c src/kern/overlayfs.c \
	src/kern/vfs.c src/kern/swap.c \
	src/kern/swap-fat.c src/kern/vm-reclaim.c src/kern/disk.c \
	drivers/loop.c \
	src/kern/partition.c src/kern/mbr-partition.c src/kern/rpi4/platform.c \
	drivers/rpi4-sdhci.c \
	src/kern/image.c src/kern/panic.c src/kern/entry.c src/kern/clock.c \
	src/kern/lock.c src/kern/waitq.c \
	src/kern/process.c src/kern/thread.c src/kern/sched.c src/kern/vmspace.c \
	src/kern/vm-object.c src/kern/vm-commit.c src/kern/filedesc.c \
	src/kern/pipe.c src/kern/cred.c \
	src/kern/signal.c \
	src/kern/cwdinfo.c \
	src/kern/elf.c src/kern/exec.c src/kern/user-probe.c src/kern/syscall.c \
	src/kern/uaccess.c src/kern/cdev.c src/kern/devfs.c \
	src/kern/console-device.c src/kern/graphics-device.c \
	src/kern/system-device.c src/kern/rpi4/unsupported-devices.c src/kern/init.c
ARM64_KERNEL_SOURCES += $(KERN_NET_SOURCES)
ARM64_KERNEL_OBJS := $(patsubst %.c,$(BUILD)/kernel/%.o,$(ARM64_KERNEL_SOURCES))
ARM64_KERNEL_LIBC_OBJS := $(patsubst %.c,$(BUILD)/kernel/%.o,$(ZEDBSD_LIBC_SOURCES))
ARM64_VMUNIX_OBJS := $(ARM64_BOOT_OBJS) $(ARM64_KERNEL_OBJS) $(ARM64_KERNEL_LIBC_OBJS)

ARM64_USER_CPPFLAGS := -nostdinc -Iinclude -Iinclude/uapi -Isrc -I. \
	-Ilibc/include -DZEDBSD_USER_ABI_AARCH64 -DZEDBSD_USER_ABI_LP64
ARM64_USER_CFLAGS := -march=armv8-a -ffreestanding -fno-pic -fno-pie \
	-fno-stack-protector -fno-asynchronous-unwind-tables -fno-unwind-tables \
	-fno-builtin -fno-common -ffunction-sections -fdata-sections \
	-Os -Wall -Wextra -Werror
ARM64_USER_RUNTIME_SOURCES := userland/libc/posix.c userland/libc/socket.c \
	userland/libc/signal.c \
	libc/heap.c libc/string.c libc/ctype.c libc/int64.c libc/strto.c \
	libc/format.c libc/stdio.c
ARM64_USER_SH_SOURCES := userland/sh/main.c userland/sh/applet.c \
	userland/sh/builtins.c
ARM64_USER_RUNTIME_OBJS := \
	$(patsubst %.c,$(BUILD)/user/%.o,$(ARM64_USER_RUNTIME_SOURCES))
ARM64_USER_SH_OBJS := \
	$(patsubst %.c,$(BUILD)/user/%.o,$(ARM64_USER_SH_SOURCES))
ARM64_USER_OBJS := $(BUILD)/user/userland/crt0-aarch64.o \
	$(ARM64_USER_RUNTIME_OBJS) $(ARM64_USER_SH_OBJS)

all: $(BUILD)/vmunix $(BUILD)/VMUNIX.A64 $(BUILD)/bin/sh
vmunix: $(BUILD)/vmunix
SH: $(BUILD)/bin/sh
POSIX-R1.ELF: $(BUILD)/POSIX-R1.ELF
arm64-image-check: $(BUILD)/VMUNIX.A64
rpi4-entry-qemu-test: $(BUILD)/VMUNIX.A64
	bash scripts/test-arm64-rpi4-entry-qemu.sh
rpi4-qemu-test: $(BUILD)/hdd-image.img
	bash scripts/test-arm64-rpi4-qemu.sh

$(BUILD)/tests/rpi4-fdt-host-test: tests/rpi4-fdt-host-test.c \
	src/hal/arm64/bsp-rpi4/fdt.c
	@mkdir -p $(dir $@)
	$(HOSTCC) -std=c11 -O2 -Wall -Wextra -Werror -Iinclude -Isrc \
		src/hal/arm64/bsp-rpi4/fdt.c $< -o $@

rpi4-fdt-host-test: $(BUILD)/tests/rpi4-fdt-host-test
	$< vendor/raspberrypi-firmware/boot/bcm2711-rpi-4-b.dtb

CHECK_RUN_TARGETS += rpi4-fdt-host-test

$(BUILD)/src/hal/arm64/%.o: src/hal/arm64/%.c
	@mkdir -p $(dir $@)
	$(ARM64_CC) $(ARM64_CPPFLAGS) $(ARM64_CFLAGS) -MMD -MP -c $< -o $@

$(BUILD)/src/hal/arm64/%.o: src/hal/arm64/%.S
	@mkdir -p $(dir $@)
	$(ARM64_CC) $(ARM64_CPPFLAGS) $(ARM64_CFLAGS) -D_ASM_SRC_ -c $< -o $@

$(BUILD)/kernel/%.o: %.c
	@mkdir -p $(dir $@)
	$(ARM64_CC) $(ARM64_CPPFLAGS) $(ARM64_CFLAGS) -fno-builtin \
		-fno-strict-aliasing -MMD -MP -c $< -o $@

$(BUILD)/kernel/libc/%.o: libc/%.c
	@mkdir -p $(dir $@)
	$(ARM64_CC) $(ARM64_CPPFLAGS) $(filter-out -mgeneral-regs-only,$(ARM64_CFLAGS)) \
		-fno-builtin -fno-strict-aliasing -MMD -MP -c $< -o $@

$(BUILD)/user/%.o: %.c
	@mkdir -p $(dir $@)
	$(ARM64_CC) $(ARM64_USER_CPPFLAGS) $(ARM64_USER_CFLAGS) \
		-fno-strict-aliasing -MMD -MP -c $< -o $@

$(BUILD)/user/userland/crt0-aarch64.o: userland/crt0-aarch64.S
	@mkdir -p $(dir $@)
	$(ARM64_CC) $(ARM64_USER_CPPFLAGS) $(ARM64_USER_CFLAGS) -c $< -o $@

$(BUILD)/bin/sh: $(ARM64_USER_OBJS) $(ARM64_PLATFORM)/user.ld \
	scripts/check-user-elf.py
	@mkdir -p $(dir $@)
	$(ARM64_LD) --gc-sections -nostdlib -static -z max-page-size=4096 \
		-z stack-size=0x100000 -T $(ARM64_PLATFORM)/user.ld \
		$(ARM64_USER_OBJS) -o $@
	@test -z "$$($(ARM64_NM) -u $@)" || { $(ARM64_NM) -u $@; exit 1; }
	$(PYTHON) scripts/check-user-elf.py --machine aarch64 $@

$(BUILD)/POSIX-R1.ELF: $(BUILD)/user/userland/crt0-aarch64.o \
	$(ARM64_USER_RUNTIME_OBJS) \
	$(BUILD)/user/userland/tests/syscall-smoke.o \
	$(ARM64_PLATFORM)/user.ld scripts/check-user-elf.py
	$(ARM64_LD) --gc-sections -nostdlib -static -z max-page-size=4096 \
		-z stack-size=0x100000 -T $(ARM64_PLATFORM)/user.ld \
		$(BUILD)/user/userland/crt0-aarch64.o \
		$(ARM64_USER_RUNTIME_OBJS) \
		$(BUILD)/user/userland/tests/syscall-smoke.o -o $@
	@test -z "$$($(ARM64_NM) -u $@)" || { $(ARM64_NM) -u $@; exit 1; }
	$(PYTHON) scripts/check-user-elf.py --machine aarch64 $@

AARCH64_ARCH_IMAGE := $(ARCH_IMAGE_DIR)/aarch64.img
AARCH64_ARCH_FILES := --file /bin/sh=$(BUILD)/bin/sh
$(eval $(call ZEDBSD_ARCH_IMAGE_RULE,$(AARCH64_ARCH_IMAGE),aarch64,$(BUILD)/bin/sh,$(AARCH64_ARCH_FILES)))
arch-image: $(AARCH64_ARCH_IMAGE)
arch-image-check: $(AARCH64_ARCH_IMAGE)-check

$(BUILD)/hdd-image.img: $(BUILD)/VMUNIX.A64 $(AARCH64_ARCH_IMAGE) \
	$(ARM64_PLATFORM)/config.txt scripts/make-rpi4-hdd-image.py \
	scripts/check-rpi4-hdd-image.py
	$(PYTHON) scripts/make-rpi4-hdd-image.py --force \
		--kernel $(BUILD)/VMUNIX.A64 --arch-image $(AARCH64_ARCH_IMAGE) \
		--config $(ARM64_PLATFORM)/config.txt \
		--firmware-dir vendor/raspberrypi-firmware/boot $@

hdd-image: $(BUILD)/hdd-image.img
rpi4-image-check: $(BUILD)/hdd-image.img
	$(PYTHON) scripts/check-rpi4-hdd-image.py --kernel $(BUILD)/VMUNIX.A64 \
		--arch-image $(AARCH64_ARCH_IMAGE) \
		--config $(ARM64_PLATFORM)/config.txt $<

$(BUILD)/vmunix: $(ARM64_VMUNIX_OBJS) $(ARM64_PLATFORM)/vmunix.ld \
	scripts/check-arm64-vmunix.py
	$(ARM64_LD) --gc-sections -z max-page-size=4096 \
		-T $(ARM64_PLATFORM)/vmunix.ld -nostdlib $(ARM64_VMUNIX_OBJS) -o $@
	@test -z "$$($(ARM64_NM) -u $@)" || { $(ARM64_NM) -u $@; exit 1; }
	$(PYTHON) scripts/check-arm64-vmunix.py --elf $@

$(BUILD)/VMUNIX.A64: $(BUILD)/vmunix scripts/check-arm64-vmunix.py
	$(ARM64_OBJCOPY) -O binary $< $@
	$(PYTHON) scripts/check-arm64-vmunix.py --elf $< --image $@ --fix-image
	$(PYTHON) scripts/check-arm64-vmunix.py --elf $< --image $@

.PHONY: vmunix SH POSIX-R1.ELF arch-image arch-image-check hdd-image rpi4-image-check arm64-image-check \
	rpi4-entry-qemu-test rpi4-fdt-host-test

.PHONY: rpi4-qemu-test

-include $(ARM64_BOOT_OBJS:.o=.d) $(ARM64_KERNEL_OBJS:.o=.d) \
	$(ARM64_KERNEL_LIBC_OBJS:.o=.d)
	
-include $(ARM64_USER_OBJS:.o=.d)

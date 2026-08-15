# zedBSD SPARC V9/sun4u bootstrap rules.
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib

SPARCV9_PREFIX ?= $(HOME)/opt/sparc64
SPARCV9_TARGET ?= sparc64-unknown-elf
SPARCV9_CC ?= $(SPARCV9_PREFIX)/bin/$(SPARCV9_TARGET)-gcc
SPARCV9_LD ?= $(SPARCV9_PREFIX)/bin/$(SPARCV9_TARGET)-ld
SPARCV9_NM ?= $(SPARCV9_PREFIX)/bin/$(SPARCV9_TARGET)-nm
SPARCV9_OBJCOPY ?= $(SPARCV9_PREFIX)/bin/$(SPARCV9_TARGET)-objcopy
SPARCV9_OBJDUMP ?= $(SPARCV9_PREFIX)/bin/$(SPARCV9_TARGET)-objdump
SPARCV9_READELF ?= $(SPARCV9_PREFIX)/bin/$(SPARCV9_TARGET)-readelf
SPARCV9_PLATFORM := platform/sparcv9

SPARCV9_CPPFLAGS := -nostdinc -Iinclude -Iinclude/uapi -Isrc -I. \
	-Ilibc/include -Isrc/hal/sparcv9 -DHAL_ARCH_SPARCV9 \
	-DHAL_BOARD_SUN4U -DZEDBSD_USER_ABI_SPARCV9 -DZEDBSD_USER_ABI_LP64 \
	-DZEDBSD_PAGE_SIZE=8192 \
	-DZEDBSD_USER_PAGE_SIZE=8192 \
	-DZEDBSD_NO_PRINTF_FLOAT \
	-DZEDBSD_INIT_PATH='"/sparcv9/bin/sh"'
SPARCV9_CFLAGS := -m64 -mcpu=ultrasparc -mstack-bias -mcmodel=medany \
	-msoft-float -mno-app-regs -ffreestanding -fno-pic -fno-pie \
	-fno-stack-protector -fno-asynchronous-unwind-tables \
	-fno-unwind-tables -fno-common -Os -Wall -Wextra -Werror

SPARCV9_EARLY_SOURCES := src/hal/sparcv9/locore.S \
	src/hal/sparcv9/trap-table.S src/hal/sparcv9/trap-entry.S \
	src/hal/sparcv9/window.S src/hal/sparcv9/context.S
SPARCV9_EARLY_C_SOURCES := src/hal/cpu-up.c src/hal/sparcv9/cmain.c \
	src/hal/sparcv9/runtime.c src/hal/sparcv9/io.c \
	src/hal/sparcv9/trap.c src/hal/sparcv9/irq.c \
	src/hal/sparcv9/timer.c src/hal/sparcv9/page.c \
	src/hal/sparcv9/space.c src/hal/sparcv9/task.c \
	src/hal/sparcv9/bsp-sun4u/boot.c \
	src/hal/sparcv9/bsp-sun4u/uart.c \
	src/hal/sparcv9/bsp-sun4u/console.c
SPARCV9_EARLY_OBJS := $(patsubst %.S,$(BUILD)/%.o,$(SPARCV9_EARLY_SOURCES)) \
	$(patsubst %.c,$(BUILD)/%.o,$(SPARCV9_EARLY_C_SOURCES))

SPARCV9_KERNEL_SOURCES := \
	src/kern/main.c src/kern/env.c src/kern/fs.c src/kern/namespace.c \
	src/kern/fat.c src/kern/fat-lfn.c src/kern/fat16.c src/kern/fat-vfs.c \
	src/kern/inode.c src/kern/file.c src/kern/namecache.c src/kern/namei.c \
	src/kern/mount.c src/kern/rootfs.c src/kern/overlayfs.c \
	src/kern/vfs.c src/kern/swap.c \
	src/kern/swap-fat.c src/kern/vm-reclaim.c src/kern/disk.c \
	drivers/loop.c \
	src/kern/partition.c src/kern/sun-disklabel.c src/kern/sun4u/platform.c \
	drivers/sun4u-cmd646.c src/kern/image.c src/kern/panic.c \
	src/kern/entry.c src/kern/clock.c src/kern/lock.c src/kern/waitq.c \
	src/kern/process.c src/kern/thread.c \
	src/kern/sched.c src/kern/vmspace.c src/kern/vm-object.c \
	src/kern/vm-commit.c src/kern/filedesc.c src/kern/pipe.c \
	src/kern/cred.c src/kern/signal.c src/kern/cwdinfo.c \
	src/kern/elf.c src/kern/exec.c \
	src/kern/user-probe.c src/kern/syscall.c src/kern/uaccess.c \
	src/kern/cdev.c src/kern/devfs.c src/kern/console-device.c \
	src/kern/graphics-device.c src/kern/system-device.c \
	src/kern/init.c
SPARCV9_KERNEL_SOURCES += $(KERN_NET_SOURCES)
SPARCV9_KERNEL_OBJS := $(patsubst %.c,$(BUILD)/kernel/%.o,$(SPARCV9_KERNEL_SOURCES))
SPARCV9_KERNEL_LIBC_OBJS := $(patsubst %.c,$(BUILD)/kernel/%.o,$(ZEDBSD_LIBC_SOURCES))
SPARCV9_VMUNIX_OBJS := $(SPARCV9_EARLY_OBJS) $(SPARCV9_KERNEL_OBJS) \
	$(SPARCV9_KERNEL_LIBC_OBJS)

SPARCV9_BOOT_CFLAGS := $(SPARCV9_CFLAGS) -fno-builtin \
	-ffunction-sections -fdata-sections
SPARCV9_STAGE1_OBJS := $(BUILD)/boot/stage1/stage1-entry.o \
	$(BUILD)/boot/stage1/stage1.o $(BUILD)/boot/stage1/ofw.o
SPARCV9_STAGE2_OBJS := $(BUILD)/boot/stage2/stage2-entry.o \
	$(BUILD)/boot/stage2/stage2.o $(BUILD)/boot/stage2/handoff.o \
	$(BUILD)/boot/stage2/ofw.o

SPARCV9_USER_CFLAGS := $(SPARCV9_CFLAGS) -fno-builtin \
	-ffunction-sections -fdata-sections
SPARCV9_USER_RUNTIME_SOURCES := userland/libc/posix.c userland/libc/socket.c \
	userland/libc/signal.c \
	libc/heap.c libc/string.c libc/ctype.c libc/int64.c libc/strto.c \
	libc/format.c libc/stdio.c
SPARCV9_USER_SH_SOURCES := userland/sh/main.c userland/sh/applet.c \
	userland/sh/builtins.c
SPARCV9_USER_RUNTIME_OBJS := \
	$(patsubst %.c,$(BUILD)/user/%.o,$(SPARCV9_USER_RUNTIME_SOURCES))
SPARCV9_USER_SH_OBJS := \
	$(patsubst %.c,$(BUILD)/user/%.o,$(SPARCV9_USER_SH_SOURCES))
SPARCV9_USER_OBJS := $(BUILD)/user/userland/crt0-sparcv9.o \
	$(SPARCV9_USER_RUNTIME_OBJS) $(SPARCV9_USER_SH_OBJS)

all: $(BUILD)/vmunix $(BUILD)/bin/sh $(BUILD)/hdd-image.img
vmunix: $(BUILD)/vmunix
SH: $(BUILD)/bin/sh
POSIX-R1.ELF: $(BUILD)/POSIX-R1.ELF

sparcv9-toolchain:
	bash scripts/build-sparcv9-toolchain.sh --prefix $(SPARCV9_PREFIX)

$(BUILD)/src/hal/cpu-up.o: src/hal/cpu-up.c
	@mkdir -p $(dir $@)
	$(SPARCV9_CC) $(SPARCV9_CPPFLAGS) $(SPARCV9_CFLAGS) \
		-MMD -MP -c $< -o $@

$(BUILD)/src/hal/sparcv9/%.o: src/hal/sparcv9/%.S
	@mkdir -p $(dir $@)
	$(SPARCV9_CC) $(SPARCV9_CPPFLAGS) $(SPARCV9_CFLAGS) \
		-D_ASM_SRC_ -c $< -o $@

$(BUILD)/src/hal/sparcv9/%.o: src/hal/sparcv9/%.c
	@mkdir -p $(dir $@)
	$(SPARCV9_CC) $(SPARCV9_CPPFLAGS) $(SPARCV9_CFLAGS) \
		-MMD -MP -c $< -o $@

$(BUILD)/kernel/%.o: %.c
	@mkdir -p $(dir $@)
	$(SPARCV9_CC) $(SPARCV9_CPPFLAGS) $(SPARCV9_CFLAGS) -fno-builtin \
		-fno-strict-aliasing -MMD -MP -c $< -o $@

$(BUILD)/kernel/libc/%.o: libc/%.c
	@mkdir -p $(dir $@)
	$(SPARCV9_CC) $(SPARCV9_CPPFLAGS) $(SPARCV9_CFLAGS) -fno-builtin \
		-fno-strict-aliasing -MMD -MP -c $< -o $@

$(BUILD)/boot/stage1/%.o: bootloader/sparcv9/%.S
	@mkdir -p $(dir $@)
	$(SPARCV9_CC) $(SPARCV9_CPPFLAGS) $(SPARCV9_BOOT_CFLAGS) \
		-D_ASM_SRC_ -c $< -o $@

$(BUILD)/boot/stage1/%.o: bootloader/sparcv9/%.c
	@mkdir -p $(dir $@)
	$(SPARCV9_CC) $(SPARCV9_CPPFLAGS) $(SPARCV9_BOOT_CFLAGS) \
		-MMD -MP -c $< -o $@

$(BUILD)/boot/stage2/%.o: bootloader/sparcv9/%.S
	@mkdir -p $(dir $@)
	$(SPARCV9_CC) $(SPARCV9_CPPFLAGS) $(SPARCV9_BOOT_CFLAGS) \
		-D_ASM_SRC_ -c $< -o $@

$(BUILD)/boot/stage2/%.o: bootloader/sparcv9/%.c
	@mkdir -p $(dir $@)
	$(SPARCV9_CC) $(SPARCV9_CPPFLAGS) $(SPARCV9_BOOT_CFLAGS) \
		-MMD -MP -c $< -o $@

$(BUILD)/user/%.o: %.c
	@mkdir -p $(dir $@)
	$(SPARCV9_CC) $(SPARCV9_CPPFLAGS) $(SPARCV9_USER_CFLAGS) \
		-fno-strict-aliasing -MMD -MP -c $< -o $@

$(BUILD)/user/userland/crt0-sparcv9.o: userland/crt0-sparcv9.S
	@mkdir -p $(dir $@)
	$(SPARCV9_CC) $(SPARCV9_CPPFLAGS) $(SPARCV9_USER_CFLAGS) \
		-c $< -o $@

$(BUILD)/vmunix: $(SPARCV9_VMUNIX_OBJS) $(SPARCV9_PLATFORM)/vmunix.ld \
	scripts/check-sparcv9-vmunix.py
	$(SPARCV9_LD) -m elf64_sparc --gc-sections \
		-z max-page-size=8192 -T $(SPARCV9_PLATFORM)/vmunix.ld \
		-nostdlib $(SPARCV9_VMUNIX_OBJS) -o $@
	@test -z "$$($(SPARCV9_NM) -u $@)" || { $(SPARCV9_NM) -u $@; exit 1; }
	$(PYTHON) scripts/check-sparcv9-vmunix.py $@

sparcv9-image-check: $(BUILD)/vmunix
	$(PYTHON) scripts/check-sparcv9-vmunix.py $<

$(BUILD)/bin/sh: $(SPARCV9_USER_OBJS) $(SPARCV9_PLATFORM)/user.ld \
	scripts/check-user-elf.py
	@mkdir -p $(dir $@)
	$(SPARCV9_CC) $(SPARCV9_USER_CFLAGS) -nostdlib -static \
		-Wl,--gc-sections -Wl,-z,max-page-size=8192 \
		-Wl,-T,$(SPARCV9_PLATFORM)/user.ld \
		$(SPARCV9_USER_OBJS) -lgcc -o $@
	@test -z "$$($(SPARCV9_NM) -u $@)" || { $(SPARCV9_NM) -u $@; exit 1; }
	$(PYTHON) scripts/check-user-elf.py --machine sparcv9 $@

$(BUILD)/POSIX-R1.ELF: $(BUILD)/user/userland/crt0-sparcv9.o \
	$(SPARCV9_USER_RUNTIME_OBJS) \
	$(BUILD)/user/userland/tests/syscall-smoke.o \
	$(SPARCV9_PLATFORM)/user.ld scripts/check-user-elf.py
	$(SPARCV9_CC) $(SPARCV9_USER_CFLAGS) -nostdlib -static \
		-Wl,--gc-sections -Wl,-z,max-page-size=8192 \
		-Wl,-T,$(SPARCV9_PLATFORM)/user.ld \
		$(BUILD)/user/userland/crt0-sparcv9.o \
		$(SPARCV9_USER_RUNTIME_OBJS) \
		$(BUILD)/user/userland/tests/syscall-smoke.o -lgcc -o $@
	@test -z "$$($(SPARCV9_NM) -u $@)" || { $(SPARCV9_NM) -u $@; exit 1; }
	$(PYTHON) scripts/check-user-elf.py --machine sparcv9 $@

$(BUILD)/boot/stage1.elf: $(SPARCV9_STAGE1_OBJS) \
	bootloader/sparcv9/stage1.ld
	$(SPARCV9_LD) -m elf64_sparc -z max-page-size=512 \
		--gc-sections -nostdlib \
		-T bootloader/sparcv9/stage1.ld $(SPARCV9_STAGE1_OBJS) -o $@

$(BUILD)/boot/stage1.bin: $(BUILD)/boot/stage1.elf
	$(SPARCV9_OBJCOPY) -O binary $< $@

$(BUILD)/boot/stage2.elf: $(SPARCV9_STAGE2_OBJS) \
	bootloader/sparcv9/stage2.ld
	$(SPARCV9_LD) -m elf64_sparc --gc-sections -nostdlib \
		-T bootloader/sparcv9/stage2.ld $(SPARCV9_STAGE2_OBJS) -o $@

$(BUILD)/boot/stage2.bin: $(BUILD)/boot/stage2.elf
	$(SPARCV9_OBJCOPY) -O binary $< $@

sparcv9-bootloader: $(BUILD)/boot/stage1.bin $(BUILD)/boot/stage2.bin
	$(PYTHON) scripts/check-sparcv9-boot.py \
		--stage1 $(BUILD)/boot/stage1.bin \
		--stage2 $(BUILD)/boot/stage2.bin

$(BUILD)/hdd-image.img: $(BUILD)/vmunix $(BUILD)/bin/sh \
	$(BUILD)/boot/stage1.bin \
	$(BUILD)/boot/stage2.bin scripts/make-sparcv9-hdd-image.py \
	scripts/check-sparcv9-hdd-image.py
	$(PYTHON) scripts/make-sparcv9-hdd-image.py --force \
		--stage1 $(BUILD)/boot/stage1.bin \
		--stage2 $(BUILD)/boot/stage2.bin \
		--kernel $(BUILD)/vmunix --shell $(BUILD)/bin/sh $@

hdd-image: $(BUILD)/hdd-image.img

sparcv9-disk-check: $(BUILD)/hdd-image.img
	$(PYTHON) scripts/check-sparcv9-hdd-image.py \
		--stage1 $(BUILD)/boot/stage1.bin \
		--stage2 $(BUILD)/boot/stage2.bin \
		--kernel $(BUILD)/vmunix --shell $(BUILD)/bin/sh $<

sparcv9-entry-qemu-test: $(BUILD)/hdd-image.img
	bash scripts/test-sparcv9-entry-qemu.sh

sparcv9-qemu-test: $(BUILD)/hdd-image.img
	bash scripts/test-sparcv9-qemu.sh

.PHONY: all vmunix SH POSIX-R1.ELF hdd-image sparcv9-toolchain sparcv9-image-check \
	sparcv9-bootloader sparcv9-disk-check sparcv9-entry-qemu-test \
	sparcv9-qemu-test

-include $(SPARCV9_EARLY_OBJS:.o=.d) \
	$(SPARCV9_STAGE1_OBJS:.o=.d) $(SPARCV9_STAGE2_OBJS:.o=.d)
-include $(SPARCV9_KERNEL_OBJS:.o=.d) $(SPARCV9_KERNEL_LIBC_OBJS:.o=.d)
-include $(SPARCV9_USER_OBJS:.o=.d)

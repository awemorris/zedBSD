# zedBSD armv7 kernel.
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib

ARMV7_PLATFORM := platform/armv7
AWK ?= awk

# The board's addresses (the board's makefile) reach the C and assembly
# sources as definitions and the link script as symbols.
ARMV7_ADDRESS_DEFINES := -DARMV7_PHYS_BASE=$(ARMV7_PHYS_BASE) \
	-DARMV7_KERNEL_PHYS=$(ARMV7_KERNEL_PHYS) \
	-DARMV7_BOOT_MAP_BYTES=$(ARMV7_BOOT_MAP_BYTES) \
	-DARMV7_LOW_RAM_SKIP=$(ARMV7_LOW_RAM_SKIP)

# The kernel and the HAL read the compiler's freestanding headers and the
# tree's include/ and src/, never the C library (as on the other ports).
# Both sides of the user interface name its ABI (include/uapi/stat.h).
ARMV7_CPPFLAGS := -nostdlibinc -Iinclude -Isrc -I. \
	-Isrc/hal/armv7 -DHAL_ARCH_ARMV7 $(ARMV7_BOARD_DEFINE) \
	-DKERN_USER_ABI_ARMV7 $(ARMV7_ADDRESS_DEFINES)
ARMV7_CPPFLAGS += $(ZEDBSD_CONFIG_CPPFLAGS)

# The kernel's C uses no floating point: the soft-float ABI without a
# floating-point unit, the ARM instruction set of the Cortex-A9, and no
# unaligned access the compiler would make up (device memory refuses it).
# The 32-bit user interface's packed structures (struct stat) hold 64-bit
# members on 4-byte boundaries, which -Wunaligned-access reports; every
# load and store ARMv7 makes of them, ldrd and strd included, needs only
# that alignment, so the warning is not taken.
ARMV7_CFLAGS := -march=armv7-a -mcpu=cortex-a9 -marm -mfloat-abi=soft -mfpu=none \
	-mno-unaligned-access -ffreestanding -fno-pic -fno-pie -fno-stack-protector \
	-fno-asynchronous-unwind-tables -fno-unwind-tables -fno-common \
	-ffunction-sections -fdata-sections -Os -Wall -Wextra -Werror \
	-Wno-unaligned-access

# Link-time optimization of vmunix: ZEDBSD_KERNEL_LTO_CFLAGS (from the top
# Makefile) goes to every C object of the kernel, its drivers and the HAL;
# the assembly stays native.
ARMV7_KERNEL_LTO_CFLAGS := $(ZEDBSD_KERNEL_LTO_CFLAGS)

ARMV7_BSP_C := $(addprefix src/hal/armv7/$(ARMV7_BSP_DIR)/,$(ARMV7_BSP_SOURCES))
ARMV7_BSP_S := $(addprefix src/hal/armv7/$(ARMV7_BSP_DIR)/,$(ARMV7_BSP_ASM_SOURCES))

ARMV7_BOOT_C := src/hal/armv7/cmain.c src/hal/armv7/bootinfo.c \
	src/hal/armv7/fdt.c src/hal/armv7/percpu.c src/hal/armv7/smp.c \
	src/hal/armv7/int.c src/hal/armv7/irq.c src/hal/armv7/gic.c \
	src/hal/armv7/clock.c src/hal/armv7/page.c src/hal/armv7/space.c \
	src/hal/armv7/task.c src/hal/armv7/debug.c src/hal/armv7/asm.c \
	src/hal/armv7/cache.c src/hal/armv7/lib.c src/hal/armv7/selftest.c \
	src/hal/armv7/fpu.c src/hal/idle-suspend-unsupported.c \
	$(ARMV7_BSP_C)
ARMV7_BOOT_S := src/hal/armv7/locore.S src/hal/armv7/vectors.S \
	src/hal/armv7/dispatch.S src/hal/armv7/aeabi.S src/hal/armv7/fpregs.S \
	$(ARMV7_BSP_S)
ARMV7_BOOT_OBJS := $(patsubst %.c,$(BUILD)/%.o,$(ARMV7_BOOT_C)) \
	$(patsubst %.S,$(BUILD)/%.o,$(ARMV7_BOOT_S))

ARMV7_KERNEL_SOURCES := \
	src/kern/main.c \
	$(KERN_FAT_SOURCES) \
	src/kern/inode.c src/kern/file.c src/kern/namecache.c src/kern/namei.c \
	src/kern/mount.c src/kern/tmpfs.c src/kern/tmpfs-pages.c src/drivers/fs/overlayfs.c \
	src/kern/vfs.c src/kern/backing-claim.c \
	src/kern/buf.c src/kern/cache.c src/kern/readahead.c src/kern/writeback.c src/kern/io.c \
	src/kern/sysctl.c src/kern/thermal.c src/kern/resource.c src/kern/poll.c src/kern/usync.c src/kern/disk.c \
	src/kern/device-io.c src/kern/irq.c src/kern/pmem.c \
	src/kern/test-checkpoint.c src/kern/text-display.c \
	src/drivers/generic/loop.c \
	src/kern/partition.c src/drivers/disklabel/mbr.c \
	$(ARMV7_PLATFORM_SOURCES) \
	src/kern/dcache.c src/kern/uncached.c \
	src/drivers/generic/fdt.c src/drivers/generic/dma.c \
	src/kern/panic.c src/kern/entry.c src/kern/clock.c \
	src/kern/timer.c src/kern/klog.c \
	src/kern/lock.c src/kern/waitq.c \
	src/kern/process.c src/kern/thread.c src/kern/sched.c src/kern/sleep.c src/kern/freeze.c \
	$(KERN_VM_SOURCES) \
	src/kern/filedesc.c src/kern/handle.c src/kern/fd-object.c \
	src/kern/record-lock.c \
	src/kern/pipe.c src/kern/cred.c \
	src/kern/signal.c \
	src/kern/cwdinfo.c \
	src/kern/elf.c src/kern/exec.c src/kern/sandbox.c src/kern/user-probe.c src/kern/syscall.c \
	src/kern/uaccess.c src/kern/cdev.c src/kern/devfs.c \
	src/drivers/generic/console.c src/drivers/generic/input.c \
	$(KERN_GPU_SOURCES) \
	$(KERN_AUDIO_SOURCES) \
	src/kern/kcrt.c src/kern/heap.c \
	src/kern/random.c src/kern/random-crypto.c \
	src/kern/tty.c src/kern/tty-csi.c \
	src/drivers/generic/system-device.c src/kern/system-event.c src/drivers/generic/memory-device.c \
	src/kern/shutdown.c \
	src/kern/init.c
ARMV7_KERNEL_SOURCES += $(KERN_NET_SOURCES) $(KERN_BLOCK_IDENTITY_SOURCES) \
	$(KERN_UFS_SOURCES)
ARMV7_KERNEL_SOURCES += $(KERN_BOOT_SOURCES)
ARMV7_KERNEL_SOURCES += $(KERN_ACL_SOURCES)
ARMV7_KERNEL_SOURCES += $(KERN_QUOTA_SOURCES)
ARMV7_KERNEL_OBJS := $(patsubst %.c,$(BUILD)/kernel/%.o,$(ARMV7_KERNEL_SOURCES))

# The Cortex-A9 divides and shifts through the run-time ABI's helpers.
# Their objects stay native: link-time optimization would drop the
# helpers before the code that calls them is generated.
ARMV7_HELPER_SOURCES := src/libc/int64.c src/hal/armv7/aeabi-divide.c
ARMV7_HELPER_OBJS := $(patsubst %.c,$(BUILD)/kernel-helper/%.o,$(ARMV7_HELPER_SOURCES))
ARMV7_KERNEL_OBJS += $(ARMV7_HELPER_OBJS)

# The kernel links no C library object: kcrt and the kernel heap supply
# what it used from libc.
ARMV7_VMUNIX_OBJS := $(ARMV7_BOOT_OBJS) $(ARMV7_KERNEL_OBJS)
$(ARMV7_VMUNIX_OBJS): $(ZEDBSD_PLATFORM_CONFIG_STAMP)
$(ARMV7_VMUNIX_OBJS): $(ZEDBSD_KERNEL_LTO_STAMP)

vmunix: $(BUILD)/vmunix $(BUILD)/vmunix.bin

# The kernel is an ELF whose segments are placed at their physical
# addresses; the loader of the emulator's virt board takes the raw image
# made from it, which it enters at its first byte.
$(BUILD)/vmunix: $(ARMV7_VMUNIX_OBJS) $(ARMV7_PLATFORM)/vmunix.ld platform/armv7/$(ZEDBSD_BOARD).mk
	$(ARMV7_LD) -m armelf --gc-sections -z max-page-size=4096 \
 --defsym=ARMV7_PHYS_BASE=$(ARMV7_PHYS_BASE) --defsym=ARMV7_KERNEL_PHYS=$(ARMV7_KERNEL_PHYS) \
 -T $(ARMV7_PLATFORM)/vmunix.ld -nostdlib $(ARMV7_VMUNIX_OBJS) -o $@
	@# A weak undefined symbol is an optional hook, not a link error.
	@test -z "$$($(ARMV7_NM) -u $@ | $(AWK) '$$1 == "U"')" || \
		{ $(ARMV7_NM) -u $@; exit 1; }

$(BUILD)/vmunix.bin: $(BUILD)/vmunix
	$(ARMV7_OBJCOPY) -O binary $< $@

$(BUILD)/src/hal/idle-suspend-unsupported.o: src/hal/idle-suspend-unsupported.c
	@mkdir -p $(dir $@)
	$(ARMV7_CC) $(ARMV7_CPPFLAGS) $(ARMV7_CFLAGS) $(ARMV7_KERNEL_LTO_CFLAGS) -MMD -MP -c $< -o $@

$(BUILD)/src/hal/armv7/%.o: src/hal/armv7/%.c
	@mkdir -p $(dir $@)
	$(ARMV7_CC) $(ARMV7_CPPFLAGS) $(ARMV7_CFLAGS) $(ARMV7_KERNEL_LTO_CFLAGS) -MMD -MP -c $< -o $@

$(BUILD)/src/hal/armv7/%.o: src/hal/armv7/%.S
	@mkdir -p $(dir $@)
	$(ARMV7_CC) $(ARMV7_CPPFLAGS) $(ARMV7_CFLAGS) -D_ASM_SRC_ -MMD -MP -c $< -o $@

$(BUILD)/kernel-helper/%.o: %.c
	@mkdir -p $(dir $@)
	$(ARMV7_CC) $(ARMV7_CPPFLAGS) $(ARMV7_CFLAGS) -fno-builtin -MMD -MP -c $< -o $@

$(BUILD)/kernel/%.o: %.c
	@mkdir -p $(dir $@)
	$(ARMV7_CC) $(ARMV7_CPPFLAGS) $(ARMV7_CFLAGS) -fno-builtin \
 -fno-strict-aliasing $(ARMV7_KERNEL_LTO_CFLAGS) -MMD -MP -c $< -o $@

-include $(ARMV7_BOOT_OBJS:.o=.d) $(ARMV7_KERNEL_OBJS:.o=.d)

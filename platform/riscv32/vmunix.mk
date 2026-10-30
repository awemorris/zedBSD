# zedBSD riscv32 kernel.
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib

RISCV32_PLATFORM := platform/riscv32
AWK ?= awk

# The board the HAL is built for.
RISCV32_BOARD_DEFINE_virt := -DHAL_BOARD_VIRT
RISCV32_BSP_DIR_virt := bsp-virt

# The kernel and the HAL read the compiler's freestanding headers and the
# tree's include/ and src/, never the C library (as on the other ports).
RISCV32_CPPFLAGS := -nostdlibinc -Iinclude -Isrc -I. \
	-Isrc/hal/riscv32 -DHAL_ARCH_RISCV32 -DKERN_ATOMIC_SERIALIZED $(RISCV32_BOARD_DEFINE_$(ZEDBSD_BOARD)) \
	-DKERN_USER_ABI_RISCV32
RISCV32_CPPFLAGS += $(ZEDBSD_CONFIG_CPPFLAGS)

# The kernel's C uses no floating point: the integer ABI, and code that may
# be linked anywhere in the address space (medany), without linker
# relaxation, which would need the global pointer.
RISCV32_CFLAGS := -march=rv32imac_zicsr_zifencei -mabi=ilp32 -mcmodel=medany -mno-relax \
	-ffreestanding -fno-pic -fno-pie -fno-stack-protector \
	-fno-asynchronous-unwind-tables -fno-unwind-tables -fno-common \
	-ffunction-sections -fdata-sections -Os -Wall -Wextra -Werror

# Link-time optimization of vmunix: ZEDBSD_KERNEL_LTO_CFLAGS (from the top
# Makefile) goes to every C object of the kernel, its drivers and the HAL;
# the assembly stays native.
RISCV32_KERNEL_LTO_CFLAGS := $(ZEDBSD_KERNEL_LTO_CFLAGS)

RISCV32_BSP := src/hal/riscv32/$(RISCV32_BSP_DIR_$(ZEDBSD_BOARD))
RISCV32_BSP_C_virt := $(RISCV32_BSP)/board.c $(RISCV32_BSP)/cons.c \
	$(RISCV32_BSP)/fwcfg.c $(RISCV32_BSP)/framebuffer.c $(RISCV32_BSP)/font.c

RISCV32_BOOT_C := src/hal/riscv32/cmain.c src/hal/riscv32/bootinfo.c \
	src/hal/riscv32/fdt.c src/hal/riscv32/sbi.c src/hal/riscv32/percpu.c \
	src/hal/riscv32/smp.c src/hal/riscv32/int.c src/hal/riscv32/irq.c \
	src/hal/riscv32/plic.c src/hal/riscv32/clock.c src/hal/riscv32/page.c \
	src/hal/riscv32/space.c src/hal/riscv32/task.c src/hal/riscv32/debug.c \
	src/hal/riscv32/asm.c src/hal/riscv32/lib.c src/hal/riscv32/atomic.c \
	src/hal/idle-suspend-unsupported.c \
	$(RISCV32_BSP_C_$(ZEDBSD_BOARD))
RISCV32_BOOT_S := src/hal/riscv32/locore.S src/hal/riscv32/trap.S \
	src/hal/riscv32/dispatch.S
RISCV32_BOOT_OBJS := $(patsubst %.c,$(BUILD)/%.o,$(RISCV32_BOOT_C)) \
	$(patsubst %.S,$(BUILD)/%.o,$(RISCV32_BOOT_S))

# The platform binding of the board.
RISCV32_PLATFORM_SOURCES_virt := src/kern/platform/hal-console.c

RISCV32_KERNEL_SOURCES := \
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
	$(RISCV32_PLATFORM_SOURCES_$(ZEDBSD_BOARD)) \
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
RISCV32_KERNEL_SOURCES += $(KERN_NET_SOURCES) $(KERN_BLOCK_IDENTITY_SOURCES) \
	$(KERN_UFS_SOURCES)
RISCV32_KERNEL_SOURCES += $(KERN_BOOT_SOURCES)
RISCV32_KERNEL_SOURCES += $(KERN_ACL_SOURCES)
RISCV32_KERNEL_SOURCES += $(KERN_QUOTA_SOURCES)
RISCV32_KERNEL_OBJS := $(patsubst %.c,$(BUILD)/kernel/%.o,$(RISCV32_KERNEL_SOURCES))

# A 32-bit hart divides and shifts 64-bit integers through the compiler's
# helper calls.  Their object stays native: link-time optimization would
# drop the helpers before the code that calls them is generated.
RISCV32_HELPER_SOURCES := src/libc/int64.c
RISCV32_HELPER_OBJS := $(patsubst %.c,$(BUILD)/kernel-helper/%.o,$(RISCV32_HELPER_SOURCES))
RISCV32_KERNEL_OBJS += $(RISCV32_HELPER_OBJS)

# The kernel links no C library object: kcrt and the kernel heap supply
# what it used from libc.
RISCV32_VMUNIX_OBJS := $(RISCV32_BOOT_OBJS) $(RISCV32_KERNEL_OBJS)
$(RISCV32_VMUNIX_OBJS): $(ZEDBSD_PLATFORM_CONFIG_STAMP)
$(RISCV32_VMUNIX_OBJS): $(ZEDBSD_KERNEL_LTO_STAMP)

vmunix: $(BUILD)/vmunix

# The kernel is an ELF the firmware's loader places by its physical
# addresses; the emulator takes it as it is.
$(BUILD)/vmunix: $(RISCV32_VMUNIX_OBJS) $(RISCV32_PLATFORM)/vmunix.ld
	$(RISCV32_LD) -m elf32lriscv --gc-sections --no-relax -z max-page-size=4096 \
 -T $(RISCV32_PLATFORM)/vmunix.ld -nostdlib $(RISCV32_VMUNIX_OBJS) -o $@
	@# A weak undefined symbol is an optional hook, not a link error.
	@test -z "$$($(RISCV32_NM) -u $@ | $(AWK) '$$1 == "U"')" || \
		{ $(RISCV32_NM) -u $@; exit 1; }

$(BUILD)/src/hal/idle-suspend-unsupported.o: src/hal/idle-suspend-unsupported.c
	@mkdir -p $(dir $@)
	$(RISCV32_CC) $(RISCV32_CPPFLAGS) $(RISCV32_CFLAGS) $(RISCV32_KERNEL_LTO_CFLAGS) -MMD -MP -c $< -o $@

$(BUILD)/src/hal/riscv32/%.o: src/hal/riscv32/%.c
	@mkdir -p $(dir $@)
	$(RISCV32_CC) $(RISCV32_CPPFLAGS) $(RISCV32_CFLAGS) $(RISCV32_KERNEL_LTO_CFLAGS) -MMD -MP -c $< -o $@

$(BUILD)/src/hal/riscv32/%.o: src/hal/riscv32/%.S
	@mkdir -p $(dir $@)
	$(RISCV32_CC) $(RISCV32_CPPFLAGS) $(RISCV32_CFLAGS) -D_ASM_SRC_ -c $< -o $@

$(BUILD)/kernel-helper/%.o: %.c
	@mkdir -p $(dir $@)
	$(RISCV32_CC) $(RISCV32_CPPFLAGS) $(RISCV32_CFLAGS) -fno-builtin -MMD -MP -c $< -o $@

$(BUILD)/kernel/%.o: %.c
	@mkdir -p $(dir $@)
	$(RISCV32_CC) $(RISCV32_CPPFLAGS) $(RISCV32_CFLAGS) -fno-builtin \
 -fno-strict-aliasing $(RISCV32_KERNEL_LTO_CFLAGS) -MMD -MP -c $< -o $@

-include $(RISCV32_BOOT_OBJS:.o=.d) $(RISCV32_KERNEL_OBJS:.o=.d)

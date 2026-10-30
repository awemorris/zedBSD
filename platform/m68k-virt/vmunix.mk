# zedBSD m68k virt kernel.
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib

# The kernel is built by the cross GCC for the MC68040, without the zedBSD
# target's compiler: the compiler's freestanding headers and the tree's
# include/ and src/, and the zedBSD target's define.
M68KV_PLATFORM := platform/m68k-virt
M68KV_CC ?= m68k-linux-gnu-gcc
M68KV_LD ?= m68k-linux-gnu-ld
M68KV_NM ?= m68k-linux-gnu-nm
AWK ?= awk

M68KV_CPPFLAGS := -nostdinc -isystem $(shell $(M68KV_CC) -print-file-name=include) \
	-Iinclude -Isrc -I. -D__ZEDBSD__=1 -D_LIBC_LIMITS_H_ \
	-DHAL_ARCH_M68K -DKERN_ATOMIC_SERIALIZED -DHAL_ARCH_M68040 -DHAL_BOARD_VIRT -DKERN_USER_ABI_M68K
M68KV_CPPFLAGS += $(ZEDBSD_CONFIG_CPPFLAGS)

# The kernel's C uses no floating point.
M68KV_CFLAGS := -m68040 -msoft-float -ffreestanding -fno-pic -fno-pie \
	-fno-stack-protector -fno-asynchronous-unwind-tables -fno-unwind-tables \
	-fno-common -ffunction-sections -fdata-sections -Os -Wall -Wextra -Werror
M68KV_ASFLAGS := -m68040 -D_ASM_SRC_

M68KV_HAL_C := src/hal/m68k/m68040/cpu.c src/hal/m68k/m68040/int.c \
	src/hal/m68k/m68040/space.c src/hal/m68k/m68040/task.c \
	src/hal/m68k/m68040/page.c src/hal/m68k/m68040/lib.c \
	src/hal/m68k/m68040/debug.c src/hal/m68k/atomic.c \
	src/hal/idle-suspend-unsupported.c \
	src/hal/m68k/bsp-virt/board.c src/hal/m68k/bsp-virt/bootinfo.c \
	src/hal/m68k/bsp-virt/irq.c src/hal/m68k/bsp-virt/clock.c \
	src/hal/m68k/bsp-virt/framebuffer.c src/hal/m68k/bsp-virt/cons.c \
	src/hal/m68k/bsp-virt/font.c
M68KV_HAL_S := src/hal/m68k/bsp-virt/start.S src/hal/m68k/m68040/entry.S
M68KV_HAL_OBJS := $(patsubst %.c,$(BUILD)/%.o,$(M68KV_HAL_C)) \
	$(patsubst %.S,$(BUILD)/%.o,$(M68KV_HAL_S))

# The platform binding: the board publishes no boot device and its console
# is the HAL's.
M68KV_KERNEL_SOURCES := \
	src/kern/main.c \
	$(KERN_FAT_SOURCES) \
	src/kern/inode.c src/kern/file.c src/kern/namecache.c src/kern/namei.c \
	src/kern/mount.c src/kern/tmpfs.c src/kern/tmpfs-pages.c src/drivers/fs/overlayfs.c \
	src/kern/vfs.c src/kern/backing-claim.c \
	src/kern/buf.c src/kern/cache.c src/kern/readahead.c src/kern/writeback.c src/kern/io.c \
	src/kern/sysctl.c src/kern/resource.c src/kern/poll.c src/kern/usync.c src/kern/disk.c \
	src/kern/device-io.c src/kern/irq.c src/kern/pmem.c \
	src/kern/test-checkpoint.c src/kern/text-display.c \
	src/drivers/generic/loop.c \
	src/kern/partition.c src/drivers/disklabel/mbr.c \
	src/kern/platform/hal-console.c \
	src/kern/dcache.c src/kern/uncached.c \
	src/drivers/generic/fdt.c src/drivers/generic/dma.c \
	src/kern/panic.c src/kern/entry.c src/kern/clock.c \
	src/kern/timer.c src/kern/klog.c \
	src/kern/lock.c src/kern/waitq.c \
	src/kern/process.c src/kern/thread.c src/kern/sched.c src/kern/sleep.c src/kern/cpuperf.c src/kern/freeze.c \
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
M68KV_KERNEL_SOURCES += $(KERN_NET_SOURCES) $(KERN_BLOCK_IDENTITY_SOURCES) \
	$(KERN_UFS_SOURCES)
M68KV_KERNEL_SOURCES += $(KERN_BOOT_SOURCES)
M68KV_KERNEL_SOURCES += $(KERN_ACL_SOURCES)
M68KV_KERNEL_SOURCES += $(KERN_QUOTA_SOURCES)
M68KV_KERNEL_OBJS := $(patsubst %.c,$(BUILD)/kernel/%.o,$(M68KV_KERNEL_SOURCES))

M68KV_VMUNIX_OBJS := $(M68KV_HAL_OBJS) $(M68KV_KERNEL_OBJS)

# The compiler's helpers for the operations the processor has no
# instruction for (64-bit division, bit counts), for the CPU and float ABI.
M68KV_LIBGCC := $(shell $(M68KV_CC) -m68040 -msoft-float -print-libgcc-file-name)
$(M68KV_VMUNIX_OBJS): $(ZEDBSD_PLATFORM_CONFIG_STAMP)

vmunix: $(BUILD)/vmunix

# The kernel is an ELF the emulator loads by its physical addresses.
$(BUILD)/vmunix: $(M68KV_VMUNIX_OBJS) $(M68KV_PLATFORM)/vmunix.ld
	$(M68KV_LD) --gc-sections -z max-page-size=4096 -T $(M68KV_PLATFORM)/vmunix.ld \
 -nostdlib $(M68KV_VMUNIX_OBJS) $(M68KV_LIBGCC) -o $@
	@test -z "$$($(M68KV_NM) -u $@ | $(AWK) '$$1 == "U"')" || \
		{ $(M68KV_NM) -u $@; exit 1; }

$(BUILD)/src/hal/%.o: src/hal/%.c
	@mkdir -p $(dir $@)
	$(M68KV_CC) $(M68KV_CPPFLAGS) $(M68KV_CFLAGS) -MMD -MP -c $< -o $@

$(BUILD)/src/hal/%.o: src/hal/%.S
	@mkdir -p $(dir $@)
	$(M68KV_CC) $(M68KV_CPPFLAGS) $(M68KV_ASFLAGS) -c $< -o $@

# GCC's flow analysis reports variables the kernel's sources set on every
# path as maybe uninitialized; that one check is left to the other ports'
# compiler.
$(BUILD)/kernel/%.o: %.c
	@mkdir -p $(dir $@)
	$(M68KV_CC) $(M68KV_CPPFLAGS) $(M68KV_CFLAGS) -fno-builtin -Wno-maybe-uninitialized \
 -fno-strict-aliasing -MMD -MP -c $< -o $@

-include $(M68KV_HAL_OBJS:.o=.d) $(M68KV_KERNEL_OBJS:.o=.d)

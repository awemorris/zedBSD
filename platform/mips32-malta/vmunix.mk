# zedBSD mips32 malta kernel.
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib

# A board's platform directory sets the four below before it includes this
# file; the malta board is the default.
MIPS32_PLATFORM ?= platform/mips32-malta
MIPS32_BOARD_CPPFLAGS ?= -DHAL_BOARD_MALTA
MIPS32_BSP ?= src/hal/mips/bsp-malta
MIPS32_BSP_C ?= $(MIPS32_BSP)/board.c $(MIPS32_BSP)/irq.c \
	$(MIPS32_BSP)/pci.c $(MIPS32_BSP)/framebuffer.c \
	$(MIPS32_BSP)/timer.c
MIPS32_BSP_S ?= $(MIPS32_BSP)/early.S
AWK ?= awk

# The project's LLVM has no zedBSD target for MIPS, so the kernel is built
# for the generic ELF target with the zedBSD target's define (the kernel
# needs no C library and no user program is built for this port yet).
MIPS32_CC ?= $(ZEDBSD_TARGET_LLVM_BIN)/clang --target=mipsel-unknown-elf -D__ZEDBSD__=1
MIPS32_LD ?= $(ZEDBSD_TARGET_LLVM_BIN)/ld.lld
MIPS32_NM ?= $(ZEDBSD_TARGET_LLVM_BIN)/llvm-nm
MIPS32_READELF ?= $(ZEDBSD_TARGET_LLVM_BIN)/llvm-readelf
MIPS32_OBJDUMP ?= $(ZEDBSD_TARGET_LLVM_BIN)/llvm-objdump

# The kernel and the HAL read the compiler's freestanding headers and the
# tree's include/ and src/, never the C library (as on the other ports).
MIPS32_CPPFLAGS := -nostdlibinc -Iinclude -Isrc -I. -Isrc/hal/mips \
	-DHAL_ARCH_MIPS32 -DKERN_ATOMIC_SERIALIZED $(MIPS32_BOARD_CPPFLAGS) -DKERN_USER_ABI_MIPS32
MIPS32_CPPFLAGS += $(ZEDBSD_CONFIG_CPPFLAGS)

# A test build of the HAL takes the paths of a virtually tagged
# instruction cache and of a secondary cache on the emulator, where they
# do nothing (WS273 p002); the default build leaves them to the
# processors that have them.
ifeq ($(CONFIG_MIPS_FORCE_VTAG_L2_PATHS),y)
MIPS32_CPPFLAGS += -DMIPS_FORCE_VTAG_L2_PATHS
endif

# The kernel's C uses no floating point.  The code is absolute, not
# position-independent, and uses no small-data section, since nothing
# sets up the global pointer those sections are reached through.
MIPS32_CFLAGS := -march=mips32r2 -mabi=32 -msoft-float -mno-abicalls -fno-pic -fno-pie \
	-G 0 -mno-gpopt -ffreestanding -fno-stack-protector \
	-fno-asynchronous-unwind-tables -fno-unwind-tables -fno-common \
	-ffunction-sections -fdata-sections -Os -Wall -Wextra -Werror

# Link-time optimization of vmunix: ZEDBSD_KERNEL_LTO_CFLAGS (from the top
# Makefile) goes to every C object of the kernel and the HAL; the assembly
# stays native.
MIPS32_KERNEL_LTO_CFLAGS := $(ZEDBSD_KERNEL_LTO_CFLAGS)

# The code generation of link-time optimization runs in the linker, which
# does not see the compiler's -mno-abicalls and -G 0: they are given to it
# again, so that the optimized object is absolute code without small data.
MIPS32_LTO_LDFLAGS := -mllvm -mattr=+noabicalls -mllvm -mips-ssection-threshold=0

MIPS32_HAL_C := src/hal/mips/cmain.c src/hal/mips/lib.c \
	src/hal/mips/int.c src/hal/mips/clock.c src/hal/mips/task.c \
	src/hal/mips/cpu.c src/hal/mips/atomic.c src/hal/mips/irq.c \
	src/hal/mips/cache.c src/hal/mips/fpu.c src/hal/mips/page.c \
	src/hal/mips/space.c src/hal/mips/debug.c src/hal/mips/selftest.c \
	src/hal/mips/cons.c src/hal/mips/font.c \
	src/hal/idle-suspend-unsupported.c \
	$(MIPS32_BSP_C)
MIPS32_HAL_S := src/hal/mips/locore.S src/hal/mips/vectors.S src/hal/mips/trap.S \
	src/hal/mips/dispatch.S src/hal/mips/fpregs.S \
	$(MIPS32_BSP_S)
MIPS32_HAL_OBJS := $(patsubst %.c,$(BUILD)/%.o,$(MIPS32_HAL_C)) \
	$(patsubst %.S,$(BUILD)/%.o,$(MIPS32_HAL_S))

# The kernel, and the platform binding that hands it the HAL's console
# (as on the other ports without a device tree).
MIPS32_KERNEL_SOURCES := \
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
	src/kern/platform/hal-console.c \
	src/kern/dcache.c src/kern/uncached.c \
	src/drivers/generic/dma.c \
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
MIPS32_KERNEL_SOURCES += $(KERN_NET_SOURCES) $(KERN_BLOCK_IDENTITY_SOURCES) \
	$(KERN_UFS_SOURCES)
MIPS32_KERNEL_SOURCES += $(KERN_BOOT_SOURCES)
MIPS32_KERNEL_SOURCES += $(KERN_ACL_SOURCES)
MIPS32_KERNEL_SOURCES += $(KERN_QUOTA_SOURCES)
MIPS32_KERNEL_OBJS := $(patsubst %.c,$(BUILD)/kernel/%.o,$(MIPS32_KERNEL_SOURCES))

# A 32-bit processor divides and shifts 64-bit integers through the
# compiler's helper calls.  Their object stays native: link-time
# optimization would drop the helpers before the code that calls them is
# generated.
MIPS32_HELPER_SOURCES := src/libc/int64.c
MIPS32_HELPER_OBJS := $(patsubst %.c,$(BUILD)/kernel-helper/%.o,$(MIPS32_HELPER_SOURCES))

MIPS32_VMUNIX_OBJS := $(MIPS32_HAL_OBJS) $(MIPS32_KERNEL_OBJS) $(MIPS32_HELPER_OBJS)
$(MIPS32_VMUNIX_OBJS): $(ZEDBSD_PLATFORM_CONFIG_STAMP)
$(MIPS32_VMUNIX_OBJS): $(ZEDBSD_KERNEL_LTO_STAMP)

vmunix: $(BUILD)/vmunix

# The kernel is an ELF the emulator's loader places by its KSEG0
# addresses; it is taken as it is.
$(BUILD)/vmunix: $(MIPS32_VMUNIX_OBJS) $(MIPS32_PLATFORM)/vmunix.ld
	$(MIPS32_LD) -m elf32ltsmip $(MIPS32_LTO_LDFLAGS) --gc-sections -z max-page-size=4096 \
 -T $(MIPS32_PLATFORM)/vmunix.ld -nostdlib $(MIPS32_VMUNIX_OBJS) -o $@
	@# A weak undefined symbol is an optional hook, not a link error.
	@test -z "$$($(MIPS32_NM) -u $@ | $(AWK) '$$1 == "U"')" || \
		{ $(MIPS32_NM) -u $@; exit 1; }
	@# No small-data section and no instruction through gp: nothing sets
	@# gp up.  (The linker keeps the two reserved words of its .got, and the
	@# trap frame saves and restores gp on the stack like any register.)
	@test -z "$$($(MIPS32_READELF) -S -W $@ | $(AWK) '/ \.(sdata|sbss|lit4|lit8)( |$$)/')" || \
		{ $(MIPS32_READELF) -S -W $@; echo "vmunix has a small-data section"; exit 1; }
	@test -z "$$($(MIPS32_OBJDUMP) -d $@ | $(AWK) '/[$$]gp/ && !/[sl]w[ \t]+[$$]gp, 0x[0-9a-f]+\([$$]sp\)/')" || \
		{ echo "vmunix has an instruction that uses gp"; exit 1; }

$(BUILD)/src/hal/idle-suspend-unsupported.o: src/hal/idle-suspend-unsupported.c
	@mkdir -p $(dir $@)
	$(MIPS32_CC) $(MIPS32_CPPFLAGS) $(MIPS32_CFLAGS) $(MIPS32_KERNEL_LTO_CFLAGS) -MMD -MP -c $< -o $@

$(BUILD)/src/hal/mips/%.o: src/hal/mips/%.c
	@mkdir -p $(dir $@)
	$(MIPS32_CC) $(MIPS32_CPPFLAGS) $(MIPS32_CFLAGS) $(MIPS32_KERNEL_LTO_CFLAGS) -MMD -MP -c $< -o $@

$(BUILD)/src/hal/mips/%.o: src/hal/mips/%.S
	@mkdir -p $(dir $@)
	$(MIPS32_CC) $(MIPS32_CPPFLAGS) $(MIPS32_CFLAGS) -D_ASM_SRC_ -c $< -o $@

$(BUILD)/kernel/%.o: %.c
	@mkdir -p $(dir $@)
	$(MIPS32_CC) $(MIPS32_CPPFLAGS) $(MIPS32_CFLAGS) -fno-builtin \
 -fno-strict-aliasing $(MIPS32_KERNEL_LTO_CFLAGS) -MMD -MP -c $< -o $@

$(BUILD)/kernel-helper/%.o: %.c
	@mkdir -p $(dir $@)
	$(MIPS32_CC) $(MIPS32_CPPFLAGS) $(MIPS32_CFLAGS) -fno-builtin -MMD -MP -c $< -o $@

.PHONY: toolchain
toolchain:
	@command -v $(firstword $(MIPS32_CC)) >/dev/null
	@command -v $(MIPS32_LD) >/dev/null
	@echo "MIPS32 toolchain: $(MIPS32_CC), $(MIPS32_LD)"

-include $(MIPS32_HAL_OBJS:.o=.d) $(MIPS32_KERNEL_OBJS:.o=.d) $(MIPS32_HELPER_OBJS:.o=.d)

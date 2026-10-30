# zedBSD mips64 malta kernel.
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib

MIPS64_PLATFORM := platform/mips64-malta
AWK ?= awk

# The project's LLVM has no zedBSD target for MIPS, so the kernel is built
# for the generic ELF target with the zedBSD target's define (the kernel
# needs no C library and no user program is built for this port yet).
MIPS64_CC ?= $(ZEDBSD_TARGET_LLVM_BIN)/clang --target=mips64el-unknown-elf -D__ZEDBSD__=1
MIPS64_LD ?= $(ZEDBSD_TARGET_LLVM_BIN)/ld.lld
MIPS64_NM ?= $(ZEDBSD_TARGET_LLVM_BIN)/llvm-nm
MIPS64_READELF ?= $(ZEDBSD_TARGET_LLVM_BIN)/llvm-readelf
MIPS64_OBJDUMP ?= $(ZEDBSD_TARGET_LLVM_BIN)/llvm-objdump

# The kernel and the HAL read the compiler's freestanding headers and the
# tree's include/ and src/, never the C library (as on the other ports).
MIPS64_CPPFLAGS := -nostdlibinc -Iinclude -Isrc -I. -Isrc/hal/mips \
	-DHAL_ARCH_MIPS64 -DHAL_BOARD_MALTA -DKERN_USER_ABI_MIPS64
MIPS64_CPPFLAGS += $(ZEDBSD_CONFIG_CPPFLAGS)

# A test build of the HAL takes the paths of a virtually tagged
# instruction cache and of a secondary cache on the emulator, where they
# do nothing (WS273 p002); the default build leaves them to the
# processors that have them.
ifeq ($(CONFIG_MIPS_FORCE_VTAG_L2_PATHS),y)
MIPS64_CPPFLAGS += -DMIPS_FORCE_VTAG_L2_PATHS
endif

# The kernel's C uses no floating point.  The code is absolute, not
# position-independent, and uses no small-data section, since nothing
# sets up the global pointer those sections are reached through.  Its
# symbols are in CKSEG0, so the compiler's sym32 feature reaches them with
# the 32-bit lui and addiu (clang has no -msym32 option; the feature is
# given to the code generator directly).
MIPS64_CFLAGS := -march=mips64r2 -mabi=64 -msoft-float -mno-abicalls -fno-pic -fno-pie \
	-Xclang -target-feature -Xclang +sym32 -mlong-calls \
	-G 0 -mno-gpopt -ffreestanding -fno-stack-protector \
	-fno-asynchronous-unwind-tables -fno-unwind-tables -fno-common \
	-ffunction-sections -fdata-sections -Os -Wall -Wextra -Werror

# Link-time optimization of vmunix: ZEDBSD_KERNEL_LTO_CFLAGS (from the top
# Makefile) goes to every C object; the assembly stays native.
MIPS64_KERNEL_LTO_CFLAGS := $(ZEDBSD_KERNEL_LTO_CFLAGS)

# The code generation of link-time optimization runs in the linker, which
# does not see the compiler's -mno-abicalls, -G 0 and sym32: they are
# given to it again, so that the optimized object is absolute code without
# small data that reaches its symbols in 32 bits.
MIPS64_LTO_LDFLAGS := -mllvm -mattr=+noabicalls,+sym32,+long-calls -mllvm -mips-ssection-threshold=0

MIPS64_BSP := src/hal/mips/bsp-malta
MIPS64_HAL_C := src/hal/mips/cmain.c src/hal/mips/lib.c \
	src/hal/mips/int.c src/hal/mips/clock.c src/hal/mips/task.c \
	src/hal/mips/cpu.c src/hal/mips/irq.c \
	src/hal/mips/cache.c src/hal/mips/fpu.c src/hal/mips/page.c \
	src/hal/mips/space.c src/hal/mips/debug.c src/hal/mips/selftest.c \
	src/hal/mips/cons.c src/hal/mips/font.c \
	src/hal/idle-suspend-unsupported.c \
	$(MIPS64_BSP)/board.c $(MIPS64_BSP)/irq.c \
	$(MIPS64_BSP)/pci.c $(MIPS64_BSP)/framebuffer.c \
	$(MIPS64_BSP)/timer.c
MIPS64_HAL_S := src/hal/mips/locore.S src/hal/mips/vectors.S src/hal/mips/trap.S \
	src/hal/mips/dispatch.S src/hal/mips/fpregs.S \
	$(MIPS64_BSP)/early.S
MIPS64_HAL_OBJS := $(patsubst %.c,$(BUILD)/%.o,$(MIPS64_HAL_C)) \
	$(patsubst %.S,$(BUILD)/%.o,$(MIPS64_HAL_S))

# The kernel, and the platform binding that hands it the HAL's console
# (as on the other ports without a device tree).
MIPS64_KERNEL_SOURCES := \
	src/kern/main.c \
	$(KERN_FAT_SOURCES) \
	src/kern/inode.c src/kern/file.c src/kern/namecache.c src/kern/namei.c \
	src/kern/mount.c src/kern/tmpfs.c src/kern/tmpfs-pages.c src/drivers/fs/overlayfs.c \
	src/kern/vfs.c src/kern/backing-claim.c \
	src/kern/buf.c src/kern/cache.c src/kern/readahead.c src/kern/writeback.c src/kern/io.c \
	src/kern/sysctl.c src/kern/thermal.c src/kern/thermal-guard.c src/kern/resource.c src/kern/poll.c src/kern/usync.c src/kern/disk.c \
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
MIPS64_KERNEL_SOURCES += $(KERN_NET_SOURCES) $(KERN_BLOCK_IDENTITY_SOURCES) \
	$(KERN_UFS_SOURCES)
MIPS64_KERNEL_SOURCES += $(KERN_BOOT_SOURCES)
MIPS64_KERNEL_SOURCES += $(KERN_ACL_SOURCES)
MIPS64_KERNEL_SOURCES += $(KERN_QUOTA_SOURCES)
MIPS64_KERNEL_OBJS := $(patsubst %.c,$(BUILD)/kernel/%.o,$(MIPS64_KERNEL_SOURCES))

MIPS64_VMUNIX_OBJS := $(MIPS64_HAL_OBJS) $(MIPS64_KERNEL_OBJS)
$(MIPS64_VMUNIX_OBJS): $(ZEDBSD_PLATFORM_CONFIG_STAMP)
$(MIPS64_VMUNIX_OBJS): $(ZEDBSD_KERNEL_LTO_STAMP)

vmunix: $(BUILD)/vmunix

# The kernel is an ELF64 the emulator's loader places by its CKSEG0
# addresses; it is taken as it is.
$(BUILD)/vmunix: $(MIPS64_VMUNIX_OBJS) $(MIPS64_PLATFORM)/vmunix.ld
	$(MIPS64_LD) -m elf64ltsmip $(MIPS64_LTO_LDFLAGS) --gc-sections -z max-page-size=4096 \
 -T $(MIPS64_PLATFORM)/vmunix.ld -nostdlib $(MIPS64_VMUNIX_OBJS) -o $@
	@# A weak undefined symbol is an optional hook, not a link error.
	@test -z "$$($(MIPS64_NM) -u $@ | $(AWK) '$$1 == "U"')" || \
		{ $(MIPS64_NM) -u $@; exit 1; }
	@# No small-data section and no instruction through gp: nothing sets
	@# gp up.
	@test -z "$$($(MIPS64_READELF) -S -W $@ | $(AWK) '/ \.(sdata|sbss|lit4|lit8)( |$$)/')" || \
		{ $(MIPS64_READELF) -S -W $@; echo "vmunix has a small-data section"; exit 1; }
	@test -z "$$($(MIPS64_OBJDUMP) -d $@ | $(AWK) '/[$$]gp/ && !/[sl]d[ \t]+[$$]gp, 0x[0-9a-f]+\([$$]sp\)/')" || \
		{ echo "vmunix has an instruction that uses gp"; exit 1; }

$(BUILD)/src/hal/mips/%.o: src/hal/mips/%.c
	@mkdir -p $(dir $@)
	$(MIPS64_CC) $(MIPS64_CPPFLAGS) $(MIPS64_CFLAGS) $(MIPS64_KERNEL_LTO_CFLAGS) -MMD -MP -c $< -o $@

$(BUILD)/src/hal/mips/%.o: src/hal/mips/%.S
	@mkdir -p $(dir $@)
	$(MIPS64_CC) $(MIPS64_CPPFLAGS) $(MIPS64_CFLAGS) -D_ASM_SRC_ -c $< -o $@

$(BUILD)/src/hal/idle-suspend-unsupported.o: src/hal/idle-suspend-unsupported.c
	@mkdir -p $(dir $@)
	$(MIPS64_CC) $(MIPS64_CPPFLAGS) $(MIPS64_CFLAGS) $(MIPS64_KERNEL_LTO_CFLAGS) -MMD -MP -c $< -o $@

$(BUILD)/kernel/%.o: %.c
	@mkdir -p $(dir $@)
	$(MIPS64_CC) $(MIPS64_CPPFLAGS) $(MIPS64_CFLAGS) -fno-builtin \
 -fno-strict-aliasing $(MIPS64_KERNEL_LTO_CFLAGS) -MMD -MP -c $< -o $@

.PHONY: toolchain
toolchain:
	@command -v $(firstword $(MIPS64_CC)) >/dev/null
	@command -v $(MIPS64_LD) >/dev/null
	@echo "MIPS64 toolchain: $(MIPS64_CC), $(MIPS64_LD)"

-include $(MIPS64_HAL_OBJS:.o=.d) $(MIPS64_KERNEL_OBJS:.o=.d)

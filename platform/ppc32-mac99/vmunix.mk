# zedBSD ppc32 mac99 kernel.
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib

PPC32_PLATFORM := platform/ppc32-mac99
AWK ?= awk

# The project's LLVM has no zedBSD target for 32-bit PowerPC, so the kernel
# is built for the generic ELF target with the zedBSD target's define (the
# kernel needs no C library and no user program is built for this port).
PPC32_CC ?= $(ZEDBSD_TARGET_LLVM_BIN)/clang --target=powerpc-unknown-elf -D__ZEDBSD__=1
PPC32_LD ?= $(ZEDBSD_TARGET_LLVM_BIN)/ld.lld
PPC32_OBJCOPY ?= $(ZEDBSD_TARGET_LLVM_BIN)/llvm-objcopy
PPC32_NM ?= $(ZEDBSD_TARGET_LLVM_BIN)/llvm-nm

PPC32_CPPFLAGS := -nostdlibinc -Iinclude -Isrc -I. -Isrc/hal/ppc32 \
	-DHAL_ARCH_PPC32 -DKERN_ATOMIC_SERIALIZED -DHAL_BOARD_MAC99 -DKERN_USER_ABI_PPC32
PPC32_CPPFLAGS += $(ZEDBSD_CONFIG_CPPFLAGS)

# The kernel's C uses no floating point and no vector unit.
PPC32_CFLAGS := -mcpu=7400 -msoft-float -mno-altivec -ffreestanding -fno-pic -fno-pie \
	-fno-stack-protector -fno-asynchronous-unwind-tables -fno-unwind-tables \
	-fno-common -ffunction-sections -fdata-sections -Os -Wall -Wextra -Werror

PPC32_HAL_C := src/hal/ppc32/cpu.c src/hal/ppc32/int.c src/hal/ppc32/space.c \
	src/hal/ppc32/task.c src/hal/ppc32/page.c src/hal/ppc32/lib.c \
	src/hal/ppc32/debug.c src/hal/ppc32/atomic.c \
	src/hal/idle-suspend-unsupported.c \
	src/hal/ppc32/bsp-mac99/board.c src/hal/ppc32/bsp-mac99/firmware.c \
	src/hal/ppc32/bsp-mac99/irq.c src/hal/ppc32/bsp-mac99/clock.c \
	src/hal/ppc32/bsp-mac99/framebuffer.c src/hal/ppc32/bsp-mac99/cons.c \
	src/hal/ppc32/bsp-mac99/font.c
PPC32_HAL_S := src/hal/ppc32/entry.S
PPC32_HAL_OBJS := $(patsubst %.c,$(BUILD)/%.o,$(PPC32_HAL_C)) \
	$(patsubst %.S,$(BUILD)/%.o,$(PPC32_HAL_S))

# The platform binding: the board publishes no boot device and its console
# is the HAL's.
PPC32_KERNEL_SOURCES := \
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
PPC32_KERNEL_SOURCES += $(KERN_NET_SOURCES) $(KERN_BLOCK_IDENTITY_SOURCES) \
	$(KERN_UFS_SOURCES)
PPC32_KERNEL_SOURCES += $(KERN_BOOT_SOURCES)
PPC32_KERNEL_SOURCES += $(KERN_ACL_SOURCES)
PPC32_KERNEL_SOURCES += $(KERN_QUOTA_SOURCES)
PPC32_KERNEL_OBJS := $(patsubst %.c,$(BUILD)/kernel/%.o,$(PPC32_KERNEL_SOURCES))

# A 32-bit processor divides and shifts 64-bit integers through the
# compiler's helper calls.
PPC32_HELPER_OBJS := $(BUILD)/kernel/src/libc/int64.o

PPC32_VMUNIX_OBJS := $(PPC32_HAL_OBJS) $(PPC32_KERNEL_OBJS) $(PPC32_HELPER_OBJS)
$(PPC32_VMUNIX_OBJS): $(ZEDBSD_PLATFORM_CONFIG_STAMP)

vmunix: $(BUILD)/vmunix.bin

# The firmware loads a flat image; the ELF is kept for the debugger.
$(BUILD)/vmunix: $(PPC32_VMUNIX_OBJS) $(PPC32_PLATFORM)/vmunix.ld
	$(PPC32_LD) -m elf32ppc --gc-sections -z max-page-size=4096 \
 -T $(PPC32_PLATFORM)/vmunix.ld -nostdlib $(PPC32_VMUNIX_OBJS) -o $@
	@test -z "$$($(PPC32_NM) -u $@ | $(AWK) '$$1 == "U"')" || \
		{ $(PPC32_NM) -u $@; exit 1; }

$(BUILD)/vmunix.bin: $(BUILD)/vmunix
	$(PPC32_OBJCOPY) -O binary $< $@

$(BUILD)/src/hal/%.o: src/hal/%.c
	@mkdir -p $(dir $@)
	$(PPC32_CC) $(PPC32_CPPFLAGS) $(PPC32_CFLAGS) -MMD -MP -c $< -o $@

$(BUILD)/src/hal/%.o: src/hal/%.S
	@mkdir -p $(dir $@)
	$(PPC32_CC) $(PPC32_CPPFLAGS) -D_ASM_SRC_ -c $< -o $@

$(BUILD)/kernel/%.o: %.c
	@mkdir -p $(dir $@)
	$(PPC32_CC) $(PPC32_CPPFLAGS) $(PPC32_CFLAGS) -fno-builtin \
 -fno-strict-aliasing -MMD -MP -c $< -o $@

-include $(PPC32_HAL_OBJS:.o=.d) $(PPC32_KERNEL_OBJS:.o=.d)

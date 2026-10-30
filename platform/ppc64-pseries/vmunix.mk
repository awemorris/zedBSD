# zedBSD ppc64 pseries kernel.
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib

PPC64_PLATFORM := platform/ppc64-pseries
AWK ?= awk

# The project's LLVM has no zedBSD target for 64-bit PowerPC, so the kernel
# is built for the generic ELF target, big-endian with the ELFv2 ABI, with
# the zedBSD target's define (the kernel needs no C library and no user
# program is built for this port).
PPC64_CC ?= $(ZEDBSD_TARGET_LLVM_BIN)/clang --target=powerpc64-unknown-elf -mabi=elfv2 -D__ZEDBSD__=1
PPC64_LD ?= $(ZEDBSD_TARGET_LLVM_BIN)/ld.lld
PPC64_NM ?= $(ZEDBSD_TARGET_LLVM_BIN)/llvm-nm

PPC64_CPPFLAGS := -nostdlibinc -Iinclude -Isrc -I. -Isrc/hal/ppc64 \
	-DHAL_ARCH_PPC64 -DKERN_ATOMIC_SERIALIZED -DHAL_BOARD_PSERIES -DKERN_USER_ABI_PPC64 -DKERN_USER_ABI_LP64
PPC64_CPPFLAGS += $(ZEDBSD_CONFIG_CPPFLAGS)

# The kernel's C uses no floating point and no vector unit.
PPC64_CFLAGS := -mcpu=power9 -msoft-float -mno-altivec -mno-vsx -ffreestanding -fno-pic -fno-pie \
	-fno-stack-protector -fno-asynchronous-unwind-tables -fno-unwind-tables \
	-fno-common -ffunction-sections -fdata-sections -Os -Wall -Wextra -Werror

PPC64_HAL_C := src/hal/ppc64/cpu.c src/hal/ppc64/int.c src/hal/ppc64/space.c \
	src/hal/ppc64/task.c src/hal/ppc64/page.c src/hal/ppc64/lib.c \
	src/hal/ppc64/debug.c src/hal/ppc64/atomic.c \
	src/hal/idle-suspend-unsupported.c \
	src/hal/ppc64/bsp-pseries/board.c src/hal/ppc64/bsp-pseries/firmware.c \
	src/hal/ppc64/bsp-pseries/irq.c src/hal/ppc64/bsp-pseries/clock.c \
	src/hal/ppc64/bsp-pseries/framebuffer.c src/hal/ppc64/bsp-pseries/cons.c \
	src/hal/ppc64/bsp-pseries/font.c
PPC64_HAL_S := src/hal/ppc64/entry.S
PPC64_HAL_OBJS := $(patsubst %.c,$(BUILD)/%.o,$(PPC64_HAL_C)) \
	$(patsubst %.S,$(BUILD)/%.o,$(PPC64_HAL_S))

# The platform binding: the board publishes no boot device and its console
# is the HAL's.
PPC64_KERNEL_SOURCES := \
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
PPC64_KERNEL_SOURCES += $(KERN_NET_SOURCES) $(KERN_BLOCK_IDENTITY_SOURCES) \
	$(KERN_UFS_SOURCES)
PPC64_KERNEL_SOURCES += $(KERN_BOOT_SOURCES)
PPC64_KERNEL_SOURCES += $(KERN_ACL_SOURCES)
PPC64_KERNEL_SOURCES += $(KERN_QUOTA_SOURCES)
PPC64_KERNEL_OBJS := $(patsubst %.c,$(BUILD)/kernel/%.o,$(PPC64_KERNEL_SOURCES))

PPC64_VMUNIX_OBJS := $(PPC64_HAL_OBJS) $(PPC64_KERNEL_OBJS)
$(PPC64_VMUNIX_OBJS): $(ZEDBSD_PLATFORM_CONFIG_STAMP)

vmunix: $(BUILD)/vmunix

# The emulator loads the ELF itself.
$(BUILD)/vmunix: $(PPC64_VMUNIX_OBJS) $(PPC64_PLATFORM)/vmunix.ld
	$(PPC64_LD) -m elf64ppc --gc-sections -z max-page-size=4096 \
 -T $(PPC64_PLATFORM)/vmunix.ld -nostdlib $(PPC64_VMUNIX_OBJS) -o $@
	@test -z "$$($(PPC64_NM) -u $@ | $(AWK) '$$1 == "U"')" || \
		{ $(PPC64_NM) -u $@; exit 1; }

$(BUILD)/src/hal/%.o: src/hal/%.c
	@mkdir -p $(dir $@)
	$(PPC64_CC) $(PPC64_CPPFLAGS) $(PPC64_CFLAGS) -MMD -MP -c $< -o $@

$(BUILD)/src/hal/%.o: src/hal/%.S
	@mkdir -p $(dir $@)
	$(PPC64_CC) $(PPC64_CPPFLAGS) -mcpu=power9 -D_ASM_SRC_ -c $< -o $@

$(BUILD)/kernel/%.o: %.c
	@mkdir -p $(dir $@)
	$(PPC64_CC) $(PPC64_CPPFLAGS) $(PPC64_CFLAGS) -fno-builtin \
 -fno-strict-aliasing -MMD -MP -c $< -o $@

-include $(PPC64_HAL_OBJS:.o=.d) $(PPC64_KERNEL_OBJS:.o=.d)

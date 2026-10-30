# zedBSD SPARC V9/sun4u kernel, boot loader, user programs and root.
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib

SPARCV9_PREFIX ?= $(if $(wildcard $(HOME)/opt/sparcv9/bin/sparc64-unknown-elf-gcc),$(HOME)/opt/sparcv9,$(HOME)/opt/sparc64)
SPARCV9_TARGET ?= sparc64-unknown-elf
SPARCV9_CC ?= $(SPARCV9_PREFIX)/bin/$(SPARCV9_TARGET)-gcc
SPARCV9_LD ?= $(SPARCV9_PREFIX)/bin/$(SPARCV9_TARGET)-ld
SPARCV9_NM ?= $(SPARCV9_PREFIX)/bin/$(SPARCV9_TARGET)-nm
SPARCV9_OBJCOPY ?= $(SPARCV9_PREFIX)/bin/$(SPARCV9_TARGET)-objcopy
SPARCV9_OBJDUMP ?= $(SPARCV9_PREFIX)/bin/$(SPARCV9_TARGET)-objdump
SPARCV9_READELF ?= $(SPARCV9_PREFIX)/bin/$(SPARCV9_TARGET)-readelf
SPARCV9_PLATFORM := platform/sparcv9
AWK ?= awk

# The cross compiler is a generic ELF GCC: it does not know the zedBSD target,
# so the build defines __ZEDBSD__ for it, as the zedBSD compilers do.  Its
# freestanding headers (stddef.h, stdint.h, stdarg.h) stay on the path.
SPARCV9_GCC_INCLUDE := $(shell $(SPARCV9_CC) -print-file-name=include 2>/dev/null)
SPARCV9_TARGET_CPPFLAGS := -nostdinc -isystem $(SPARCV9_GCC_INCLUDE) -D__ZEDBSD__=1

# The kernel and the HAL read the compiler's freestanding headers and the
# tree's include/ and src/, never the C library (as on the other ports).
SPARCV9_CPPFLAGS := $(SPARCV9_TARGET_CPPFLAGS) -Iinclude -Isrc -I. \
	-Isrc/hal/sparcv9 -DHAL_ARCH_SPARCV9 \
	-DHAL_BOARD_SUN4U -DKERN_USER_ABI_SPARCV9 -DKERN_USER_ABI_LP64 \
	-DKERN_PAGE_SIZE=8192 \
	-DKERN_USER_PAGE_SIZE=8192 \
	-DKERN_NO_PRINTF_FLOAT
SPARCV9_CPPFLAGS += $(ZEDBSD_CONFIG_CPPFLAGS)
SPARCV9_CFLAGS := -m64 -mcpu=ultrasparc -mstack-bias -mcmodel=medany \
	-msoft-float -mno-app-regs -ffreestanding -fno-pic -fno-pie \
	-fno-stack-protector -fno-asynchronous-unwind-tables \
	-fno-unwind-tables -fno-common -ffunction-sections -fdata-sections \
	-Os -Wall -Wextra -Werror

SPARCV9_EARLY_SOURCES := src/hal/sparcv9/locore.S \
	src/hal/sparcv9/trap-table.S src/hal/sparcv9/trap-entry.S \
	src/hal/sparcv9/window.S src/hal/sparcv9/context.S
SPARCV9_EARLY_C_SOURCES := src/hal/cpu-up.c src/hal/idle-suspend-unsupported.c src/hal/sparcv9/cmain.c \
	src/hal/sparcv9/runtime.c src/hal/sparcv9/io.c \
	src/hal/sparcv9/trap.c src/hal/sparcv9/irq.c \
	src/hal/sparcv9/timer.c src/hal/sparcv9/page.c \
	src/hal/sparcv9/space.c src/hal/sparcv9/task.c \
	src/hal/sparcv9/bsp-sun4u/boot.c \
	src/hal/sparcv9/bsp-sun4u/uart.c \
	src/hal/sparcv9/bsp-sun4u/console.c \
	src/hal/sparcv9/bsp-sun4u/framebuffer.c \
	src/hal/sparcv9/bsp-sun4u/font.c
SPARCV9_EARLY_OBJS := $(patsubst %.S,$(BUILD)/%.o,$(SPARCV9_EARLY_SOURCES)) \
	$(patsubst %.c,$(BUILD)/%.o,$(SPARCV9_EARLY_C_SOURCES))

SPARCV9_KERNEL_SOURCES := \
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
	src/kern/partition.c src/drivers/disklabel/sun.c \
	src/kern/platform/sun4u.c \
	src/drivers/platform/sun4u/sun4u-cmd646.c \
	src/drivers/platform/sun4u/sun4u-console.c src/kern/panic.c \
	src/drivers/platform/sun4u/sun4u-firmware-fb.c \
	src/drivers/platform/pcat/graphics/vgafont.c \
	src/drivers/isa/ps2-8042.c \
	src/kern/entry.c src/kern/clock.c src/kern/timer.c src/kern/klog.c \
	src/kern/lock.c src/kern/waitq.c \
	src/kern/process.c src/kern/thread.c \
	src/kern/sched.c src/kern/sleep.c src/kern/cpuperf.c src/kern/freeze.c $(KERN_VM_SOURCES) \
 src/kern/filedesc.c src/kern/handle.c src/kern/fd-object.c src/kern/pipe.c \
	src/kern/record-lock.c \
	src/kern/cred.c src/kern/signal.c src/kern/cwdinfo.c \
	src/kern/elf.c src/kern/exec.c src/kern/sandbox.c \
	src/kern/user-probe.c src/kern/syscall.c src/kern/uaccess.c \
	src/kern/cdev.c src/kern/devfs.c src/drivers/generic/console.c \
	src/drivers/generic/input.c \
	$(KERN_GPU_SOURCES) \
	$(KERN_AUDIO_SOURCES) \
	src/kern/kcrt.c src/kern/heap.c \
	src/kern/random.c src/kern/random-crypto.c \
	src/kern/tty.c src/kern/tty-csi.c \
	src/drivers/generic/system-device.c src/kern/system-event.c src/drivers/generic/memory-device.c src/kern/shutdown.c src/kern/init.c
SPARCV9_KERNEL_SOURCES += $(KERN_NET_SOURCES) \
	$(KERN_BLOCK_IDENTITY_SOURCES) $(KERN_UFS_SOURCES)
SPARCV9_KERNEL_SOURCES += $(KERN_BOOT_SOURCES)
SPARCV9_KERNEL_SOURCES += $(KERN_ACL_SOURCES)
SPARCV9_KERNEL_SOURCES += $(KERN_QUOTA_SOURCES)
# /dev/graphics over the firmware framebuffer (WS275).
ifeq ($(CONFIG_DRIVER_GRAPHICS_DEVICE),y)
SPARCV9_KERNEL_SOURCES += src/drivers/generic/graphics-device.c
endif
SPARCV9_KERNEL_OBJS := $(patsubst %.c,$(BUILD)/kernel/%.o,$(SPARCV9_KERNEL_SOURCES))

# The kernel links no C library object: kcrt and the kernel heap supply
# what it used from libc.
SPARCV9_VMUNIX_OBJS := $(SPARCV9_EARLY_OBJS) $(SPARCV9_KERNEL_OBJS)
$(SPARCV9_VMUNIX_OBJS): $(ZEDBSD_PLATFORM_CONFIG_STAMP) \
	$(ZEDBSD_GRAPHICS_CONFIG_STAMP)

SPARCV9_BOOT_CFLAGS := $(SPARCV9_CFLAGS) -fno-builtin \
	-ffunction-sections -fdata-sections
SPARCV9_STAGE1_OBJS := $(BUILD)/boot/stage1/stage1-entry.o \
	$(BUILD)/boot/stage1/stage1.o $(BUILD)/boot/stage1/ofw.o
SPARCV9_STAGE2_OBJS := $(BUILD)/boot/stage2/stage2-entry.o \
	$(BUILD)/boot/stage2/stage2.o $(BUILD)/boot/stage2/handoff.o \
	$(BUILD)/boot/stage2/ofw.o

# The user programs are static.  The cross compiler has no zedBSD sysroot,
# so the C library of the sysroots (toolchain/llvm/sysroot.mk) is built here
# from the same sources into one archive, with the compiler runtime and the
# SPARC V9 binary128 long double the ABI uses.
#
# The generic ELF compiler's wchar_t is 16 bits; zedBSD's is a 32-bit int,
# as the other ports' compilers define it, so the predefined macros the
# headers read are set to that.  Nothing in the base system writes a wide
# string literal, whose elements the compiler would still make 16 bits.
SPARCV9_USER_WCHAR_CPPFLAGS := -U__WCHAR_TYPE__ -D__WCHAR_TYPE__=int \
	-U__WCHAR_MAX__ -D__WCHAR_MAX__=0x7fffffff \
	-U__WCHAR_MIN__ '-D__WCHAR_MIN__=(-0x7fffffff - 1)' \
	-U__WCHAR_WIDTH__ -D__WCHAR_WIDTH__=32 \
	-U__SIZEOF_WCHAR_T__ -D__SIZEOF_WCHAR_T__=4
SPARCV9_USER_CPPFLAGS := $(SPARCV9_TARGET_CPPFLAGS) $(SPARCV9_USER_WCHAR_CPPFLAGS) \
	-Iinclude/libc -Iinclude \
	-Isrc -I. -DHAL_ARCH_SPARCV9 -DKERN_USER_ABI_SPARCV9 -DKERN_USER_ABI_LP64 \
	-DKERN_USER_PAGE_SIZE=8192 -DZEDBSD_STATIC_TLS
SPARCV9_USER_CFLAGS := $(SPARCV9_CFLAGS) -fno-builtin -fno-strict-aliasing
SPARCV9_USER_RUNTIME_SOURCES := $(sort $(ZEDBSD_SYSROOT_LIBC_SOURCES) \
	$(ZEDBSD_SYSROOT_COMPILER_RT_SOURCES) \
	src/libc/softfloat128.c src/libc/compiler-runtime128.c \
	src/libc/compiler-runtime-sparcv9.c)
SPARCV9_USER_RUNTIME_OBJS := \
	$(patsubst %.c,$(BUILD)/user/%.o,$(SPARCV9_USER_RUNTIME_SOURCES))
SPARCV9_USER_CRT0 := $(BUILD)/user/src/libc/crt/crt0-sparcv9.o
SPARCV9_USER_LIBC := $(BUILD)/lib/libc-sparcv9.a
SPARCV9_USER_READLINE_OBJ := $(BUILD)/user/userland/base/libedit/readline.o
SPARCV9_USER_COMMON_OBJS := $(BUILD)/user/userland/base/common/command.o \
	$(BUILD)/user/userland/base/common/pager.o

# A program is the start file, its own objects and the C library.
SPARCV9_USER_LINK = $(SPARCV9_LD) -m elf64_sparc --gc-sections -nostdlib \
 -static -z max-page-size=8192 -T $(SPARCV9_PLATFORM)/user.ld $(SPARCV9_USER_CRT0)
SPARCV9_USER_INPUTS := $(SPARCV9_USER_CRT0) $(SPARCV9_USER_LIBC) \
	$(SPARCV9_PLATFORM)/user.ld tools/build/check-user-elf.py
SPARCV9_USER_CHECK = $(PYTHON) tools/build/check-user-elf.py --machine sparcv9

vmunix: $(BUILD)/vmunix

$(BUILD)/src/hal/cpu-up.o: src/hal/cpu-up.c
	@mkdir -p $(dir $@)
	$(SPARCV9_CC) $(SPARCV9_CPPFLAGS) $(SPARCV9_CFLAGS) \
 -MMD -MP -c $< -o $@

$(BUILD)/src/hal/idle-suspend-unsupported.o: src/hal/idle-suspend-unsupported.c
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

$(BUILD)/vmunix: $(SPARCV9_VMUNIX_OBJS) $(ZEDBSD_GRAPHICS_CONFIG_STAMP) \
	$(SPARCV9_PLATFORM)/vmunix.ld \
	platform/sparcv9/tools/check-sparcv9-vmunix.py
	$(SPARCV9_LD) -m elf64_sparc --gc-sections \
 -z max-page-size=8192 -T $(SPARCV9_PLATFORM)/vmunix.ld \
 -nostdlib $(SPARCV9_VMUNIX_OBJS) -o $@
	@# A weak undefined symbol is an optional hook, not a link error.
	@test -z "$$($(SPARCV9_NM) -u $@ | $(AWK) '$$1 == "U"')" || \
		{ $(SPARCV9_NM) -u $@; exit 1; }
	$(PYTHON) platform/sparcv9/tools/check-sparcv9-vmunix.py $@

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

$(BUILD)/user/%.o: %.c
	@mkdir -p $(dir $@)
	$(SPARCV9_CC) $(SPARCV9_USER_CPPFLAGS) $(SPARCV9_USER_CFLAGS) \
 -MMD -MP -c $< -o $@

$(SPARCV9_USER_CRT0): src/libc/crt/crt0-sparcv9.S \
	include/hal/arch.h include/hal/arch/sparcv9.h
	@mkdir -p $(dir $@)
	$(SPARCV9_CC) $(SPARCV9_USER_CPPFLAGS) $(SPARCV9_USER_CFLAGS) \
 -c $< -o $@

$(SPARCV9_USER_LIBC): $(SPARCV9_USER_RUNTIME_OBJS)
	@mkdir -p $(dir $@)
	$(SPARCV9_PREFIX)/bin/$(SPARCV9_TARGET)-ar rcsD $@ $^

# The shell, the system control tool and mount have rules of their own;
# every other base program is its objects with the shared command helpers.
SPARCV9_USER_SH_OBJS := $(call ZEDBSD_USERLAND_OBJECTS,$(BUILD)/user,sh) \
	$(SPARCV9_USER_READLINE_OBJ)
$(SPARCV9_USER_SH_OBJS): SPARCV9_USER_CPPFLAGS += -Iuserland/base/libedit

$(BUILD)/bin/sh: $(SPARCV9_USER_INPUTS) $(SPARCV9_USER_SH_OBJS)
	@mkdir -p $(dir $@)
	$(SPARCV9_USER_LINK) $(SPARCV9_USER_SH_OBJS) $(SPARCV9_USER_LIBC) -o $@
	$(SPARCV9_USER_CHECK) $@

$(BUILD)/bin/sysctl: $(SPARCV9_USER_INPUTS) \
	$(BUILD)/user/userland/base/sysctl/main.o
	@mkdir -p $(dir $@)
	$(SPARCV9_USER_LINK) $(BUILD)/user/userland/base/sysctl/main.o \
 $(SPARCV9_USER_LIBC) -o $@
	$(SPARCV9_USER_CHECK) $@

$(BUILD)/bin/mount: $(SPARCV9_USER_INPUTS) \
	$(BUILD)/user/userland/base/mount/main.o
	@mkdir -p $(dir $@)
	$(SPARCV9_USER_LINK) $(BUILD)/user/userland/base/mount/main.o \
 $(SPARCV9_USER_LIBC) -o $@
	$(SPARCV9_USER_CHECK) $@

$(BUILD)/bin/umount: $(BUILD)/bin/mount
	@mkdir -p $(dir $@)
	cp -f $< $@

define SPARCV9_USER_BASIC_COMMAND
$(BUILD)/bin/$(1): $(SPARCV9_USER_INPUTS) $(SPARCV9_USER_COMMON_OBJS) \
	$(call ZEDBSD_USERLAND_OBJECTS,$(BUILD)/user,$(1))
	@mkdir -p $$(dir $$@)
	$(SPARCV9_USER_LINK) $(SPARCV9_USER_COMMON_OBJS) \
 $(call ZEDBSD_USERLAND_OBJECTS,$(BUILD)/user,$(1)) $(SPARCV9_USER_LIBC) -o $$@
	$(SPARCV9_USER_CHECK) $$@
endef
$(foreach command,$(USERLAND_SELECTED_BASIC_PROGRAMS),\
	$(eval $(call SPARCV9_USER_BASIC_COMMAND,$(command))))

# The root is static, and holds the base programs; the network programs
# are left out, as nothing here drives a network device.
ZEDBSD_ROOTFS_DYNAMIC := n
include platform/common/userland-rootfs.mk
SPARCV9_ROOTFS_COMMANDS := $(sort sh sysctl mount umount \
	$(USERLAND_SELECTED_BASIC_PROGRAMS))
SPARCV9_ROOTFS_INPUTS := $(addprefix $(BUILD)/bin/,$(SPARCV9_ROOTFS_COMMANDS)) \
	$(ZEDBSD_ACCOUNT_INPUTS) $(ZEDBSD_BASE_DATA_INPUTS)
SPARCV9_ROOTFS_FILES := $(foreach command,$(SPARCV9_ROOTFS_COMMANDS),\
 --file $(if $(filter umount,$(command)),/sbin/umount,$(call zedbsd_userland_destination,$(command)))=$(BUILD)/bin/$(command)) \
	$(foreach program,$(SPARCV9_ROOTFS_COMMANDS),$(if $(USERLAND_$(program)_MODE),\
 --mode $(call zedbsd_userland_destination,$(program))=$(USERLAND_$(program)_MODE))) \
	$(ZEDBSD_ACCOUNT_FILES) $(ZEDBSD_BASE_DATA_FILES)
$(eval $(call ZEDBSD_ROOTFS_TREE_RULE,sparcv9,$(SPARCV9_ROOTFS_INPUTS),$(SPARCV9_ROOTFS_FILES)))

rootfs: $(BUILD)/rootfs/.stamp

# The root partition: a UFS image of the root tree.
$(BUILD)/ufs-root.img: $(BUILD)/rootfs/.stamp \
	tools/build/make-ufs-root-image.py tools/build/ufs_format.py
	$(PYTHON) tools/build/make-ufs-root-image.py --force \
 --arch-profile sparcv9 --tree $(BUILD)/rootfs --size-mib 60 $@

# The disk: the Sun label, the two boot stages, the FAT slice with the
# kernel, and the UFS root in slice d.
$(BUILD)/hdd-image.img: $(BUILD)/vmunix $(BUILD)/ufs-root.img \
	$(BUILD)/boot/stage1.bin $(BUILD)/boot/stage2.bin \
	platform/sparcv9/tools/make-sparcv9-hdd-image.py \
	platform/sparcv9/tools/check-sparcv9-hdd-image.py
	$(PYTHON) platform/sparcv9/tools/make-sparcv9-hdd-image.py --force \
 --stage1 $(BUILD)/boot/stage1.bin \
 --stage2 $(BUILD)/boot/stage2.bin \
 --kernel $(BUILD)/vmunix \
 --ufs-root $(BUILD)/ufs-root.img $@

-include $(SPARCV9_EARLY_OBJS:.o=.d) \
	$(SPARCV9_STAGE1_OBJS:.o=.d) $(SPARCV9_STAGE2_OBJS:.o=.d)
-include $(SPARCV9_KERNEL_OBJS:.o=.d)
-include $(SPARCV9_USER_RUNTIME_OBJS:.o=.d)

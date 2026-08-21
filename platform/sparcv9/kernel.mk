# zedBSD SPARC V9/sun4u bootstrap rules.
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

SPARCV9_CPPFLAGS := -nostdinc -Iinclude -Iinclude/uapi -Isrc -I. \
	-Ilibc/include -Isrc/hal/sparcv9 -DHAL_ARCH_SPARCV9 \
	-DHAL_BOARD_SUN4U -DZEDBSD_USER_ABI_SPARCV9 -DZEDBSD_USER_ABI_LP64 \
	-DZEDBSD_PAGE_SIZE=8192 \
	-DZEDBSD_USER_PAGE_SIZE=8192 \
	-DZEDBSD_NO_PRINTF_FLOAT \
	-DZEDBSD_INIT_PATH='"/bin/sh"'
SPARCV9_CPPFLAGS += $(ZEDBSD_CONFIG_CPPFLAGS)
SPARCV9_CFLAGS := -m64 -mcpu=ultrasparc -mstack-bias -mcmodel=medany \
	-msoft-float -mno-app-regs -ffreestanding -fno-pic -fno-pie \
	-fno-stack-protector -fno-asynchronous-unwind-tables \
	-fno-unwind-tables -fno-common -Os -Wall -Wextra -Werro

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
	src/kern/mount.c src/kern/rootfs.c src/kern/tmpfs.c src/kern/overlayfs.c \
	src/kern/vfs.c src/kern/swap.c \
	src/kern/swap-fat.c src/kern/vm-reclaim.c src/kern/buf.c \
	src/kern/sysctl.c src/kern/resource.c src/kern/poll.c src/kern/usync.c src/kern/disk.c \
	src/kern/resource-limit.c \
	drivers/loop.c \
	src/kern/partition.c src/kern/sun-disklabel.c src/kern/sun4u/platform.c \
	drivers/sun4u-cmd646.c src/kern/image.c src/kern/panic.c \
	src/kern/entry.c src/kern/clock.c src/kern/process-timer.c src/kern/klog.c \
	src/kern/lock.c src/kern/waitq.c \
	src/kern/process.c src/kern/thread.c \
	src/kern/sched.c src/kern/vmspace.c src/kern/vm-object.c \
	src/kern/vm-commit.c src/kern/filedesc.c src/kern/pipe.c \
	src/kern/record-lock.c \
	src/kern/cred.c src/kern/signal.c src/kern/cwdinfo.c \
	src/kern/elf.c src/kern/exec.c \
	src/kern/user-probe.c src/kern/syscall.c src/kern/uaccess.c \
	src/kern/cdev.c src/kern/devfs.c src/kern/console-device.c \
	src/kern/mouse-device.c src/kern/tty.c \
	src/kern/graphics-device.c src/kern/system-device.c \
	src/kern/init.c
SPARCV9_KERNEL_SOURCES += $(KERN_NET_SOURCES) $(KERN_UFS1_SOURCES) \
	$(KERN_UFS2_SOURCES) $(KERN_UFS_CONSISTENCY_SOURCES)
SPARCV9_KERNEL_SOURCES += $(KERN_ACL_SOURCES)
SPARCV9_KERNEL_SOURCES += $(KERN_QUOTA_SOURCES)
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
SPARCV9_USER_RUNTIME_SOURCES := userland/base/libc/posix.c userland/base/libc/dlfcn.c userland/base/libc/static-tls.c userland/base/libc/poll.c \
	userland/base/libc/termios.c \
	userland/base/libc/pthread.c \
	userland/base/libc/shm.c \
	userland/base/libc/semaphore.c \
	userland/base/libc/mqueue.c \
	userland/base/libc/socket.c \
	userland/base/libc/signal.c userland/base/libc/account.c userland/base/libc/crypt.c \
	userland/base/libc/utmpx.c \
	libc/heap.c libc/string.c libc/ctype.c libc/locale.c libc/wide.c libc/int64.c libc/strto.c \
	libc/format.c libc/stdio.c
SPARCV9_USER_SH_SOURCES := userland/base/sh/main.c \
	userland/base/sh/builtins.c userland/base/sh/lexer.c userland/base/sh/expand.c

SPARCV9_USER_SH_SOURCES += userland/base/sh/glob.c userland/base/sh/vars.c \
	userland/base/sh/arithmetic.c

SPARCV9_USER_SH_SOURCES += userland/base/sh/alias.c
SPARCV9_USER_RUNTIME_OBJS := \
	$(patsubst %.c,$(BUILD)/user/%.o,$(SPARCV9_USER_RUNTIME_SOURCES))
SPARCV9_USER_SH_OBJS := \
	$(patsubst %.c,$(BUILD)/user/%.o,$(SPARCV9_USER_SH_SOURCES))
SPARCV9_USER_READLINE_OBJ := $(BUILD)/user/userland/base/libedit/readline.o
SPARCV9_USER_READLINE_LIB := $(BUILD)/lib/libreadline.a
SPARCV9_USER_OBJS := $(BUILD)/user/src/crt/crt0-sparcv9.o \
	$(SPARCV9_USER_RUNTIME_OBJS) $(SPARCV9_USER_SH_OBJS)

all: $(BUILD)/vmunix $(BUILD)/bin/sh $(BUILD)/bin/sysctl \
	$(BUILD)/bin/mount $(BUILD)/bin/umount \
	$(BUILD)/hdd-image.img
vmunix: $(BUILD)/vmunix
SH: $(BUILD)/bin/sh
POSIX-R1.ELF: $(BUILD)/POSIX-R1.ELF
POSIX-R2.ELF: $(BUILD)/POSIX-R2.ELF
POSIX-R2-REMAINING.ELF: $(BUILD)/POSIX-R2-REMAINING.ELF

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

$(BUILD)/user/src/crt/crt0-sparcv9.o: src/crt/crt0-sparcv9.S
	@mkdir -p $(dir $@)
	$(SPARCV9_CC) $(SPARCV9_CPPFLAGS) $(SPARCV9_USER_CFLAGS) \
		-c $< -o $@

$(SPARCV9_USER_SH_OBJS) $(SPARCV9_USER_READLINE_OBJ): \
	SPARCV9_CPPFLAGS += -Iuserland/base/libedit
$(SPARCV9_USER_READLINE_LIB): $(SPARCV9_USER_READLINE_OBJ)
	@mkdir -p $(dir $@)
	$(AR) rcs $@ $^

$(BUILD)/vmunix: $(SPARCV9_VMUNIX_OBJS) $(SPARCV9_PLATFORM)/vmunix.ld \
	tools/build/check-sparcv9-vmunix.py
	$(SPARCV9_LD) -m elf64_sparc --gc-sections \
		-z max-page-size=8192 -T $(SPARCV9_PLATFORM)/vmunix.ld \
		-nostdlib $(SPARCV9_VMUNIX_OBJS) -o $@
	@test -z "$$($(SPARCV9_NM) -u $@)" || { $(SPARCV9_NM) -u $@; exit 1; }
	$(PYTHON) tools/build/check-sparcv9-vmunix.py $@

sparcv9-image-check: $(BUILD)/vmunix
	$(PYTHON) tools/build/check-sparcv9-vmunix.py $<

$(BUILD)/bin/sh: $(SPARCV9_USER_OBJS) $(SPARCV9_USER_READLINE_LIB) \
	$(SPARCV9_PLATFORM)/user.ld \
	tools/build/check-user-elf.py
	@mkdir -p $(dir $@)
	$(SPARCV9_CC) $(SPARCV9_USER_CFLAGS) -nostdlib -static \
		-Wl,--gc-sections -Wl,-z,max-page-size=8192 \
		-Wl,-T,$(SPARCV9_PLATFORM)/user.ld \
		$(SPARCV9_USER_OBJS) $(SPARCV9_USER_READLINE_LIB) -lgcc -o $@
	@test -z "$$($(SPARCV9_NM) -u $@)" || { $(SPARCV9_NM) -u $@; exit 1; }
	$(PYTHON) tools/build/check-user-elf.py --machine sparcv9 $@

SPARCV9_USER_SYSCTL_OBJ := $(BUILD)/user/userland/base/sysctl/main.o
$(BUILD)/bin/sysctl: $(BUILD)/user/src/crt/crt0-sparcv9.o \
	$(SPARCV9_USER_RUNTIME_OBJS) $(SPARCV9_USER_SYSCTL_OBJ) \
	$(SPARCV9_PLATFORM)/user.ld tools/build/check-user-elf.py
	@mkdir -p $(dir $@)
	$(SPARCV9_CC) $(SPARCV9_USER_CFLAGS) -nostdlib -static \
		-Wl,--gc-sections -Wl,-z,max-page-size=8192 \
		-Wl,-T,$(SPARCV9_PLATFORM)/user.ld \
		$(BUILD)/user/src/crt/crt0-sparcv9.o \
		$(SPARCV9_USER_RUNTIME_OBJS) $(SPARCV9_USER_SYSCTL_OBJ) \
		-lgcc -o $@
	@test -z "$$($(SPARCV9_NM) -u $@)" || { $(SPARCV9_NM) -u $@; exit 1; }
	$(PYTHON) tools/build/check-user-elf.py --machine sparcv9 $@

SPARCV9_USER_MOUNT_OBJ := $(BUILD)/user/userland/base/mount/main.o
$(BUILD)/bin/mount: $(BUILD)/user/src/crt/crt0-sparcv9.o \
	$(SPARCV9_USER_RUNTIME_OBJS) $(SPARCV9_USER_MOUNT_OBJ) \
	$(SPARCV9_PLATFORM)/user.ld tools/build/check-user-elf.py
	@mkdir -p $(dir $@)
	$(SPARCV9_CC) $(SPARCV9_USER_CFLAGS) -nostdlib -static \
		-Wl,--gc-sections -Wl,-z,max-page-size=8192 \
		-Wl,-T,$(SPARCV9_PLATFORM)/user.ld \
		$(BUILD)/user/src/crt/crt0-sparcv9.o \
		$(SPARCV9_USER_RUNTIME_OBJS) $(SPARCV9_USER_MOUNT_OBJ) \
		-lgcc -o $@
	@test -z "$$($(SPARCV9_NM) -u $@)" || { $(SPARCV9_NM) -u $@; exit 1; }
	$(PYTHON) tools/build/check-user-elf.py --machine sparcv9 $@
$(BUILD)/bin/umount: $(BUILD)/bin/mount
	@mkdir -p $(dir $@)
	cp -f $< $@

USER_BASIC_COMMANDS := $(filter $(ZEDBSD_USER_PROGRAMS),$(USERLAND_BASIC_PROGRAMS))
USER_BASIC_TARGETS := $(addprefix $(BUILD)/bin/,$(USER_BASIC_COMMANDS))
SPARCV9_USER_BASIC_COMMON_OBJ := $(BUILD)/user/userland/base/common/command.o $(BUILD)/user/userland/base/common/pager.o

define SPARCV9_USER_BASIC_COMMAND
$(BUILD)/bin/$(1): $(BUILD)/user/src/crt/crt0-sparcv9.o \
	$(SPARCV9_USER_RUNTIME_OBJS) $(SPARCV9_USER_BASIC_COMMON_OBJ) \
	$(call ZEDBSD_USERLAND_OBJECTS,$(BUILD)/user,$(1)) $(SPARCV9_PLATFORM)/user.ld \
	tools/build/check-user-elf.py
	@mkdir -p $$(dir $$@)
	$(SPARCV9_CC) $(SPARCV9_USER_CFLAGS) -nostdlib -static \
		-Wl,--gc-sections -Wl,-z,max-page-size=8192 \
		-Wl,-T,$(SPARCV9_PLATFORM)/user.ld \
		$(BUILD)/user/src/crt/crt0-sparcv9.o \
		$(SPARCV9_USER_RUNTIME_OBJS) $(SPARCV9_USER_BASIC_COMMON_OBJ) \
		$(call ZEDBSD_USERLAND_OBJECTS,$(BUILD)/user,$(1)) -lgcc -o $$@
	@test -z "$$$$($(SPARCV9_NM) -u $$@)" || { $(SPARCV9_NM) -u $$@; exit 1; }
	$(PYTHON) tools/build/check-user-elf.py --machine sparcv9 $$@
endef
$(foreach command,$(USER_BASIC_COMMANDS),\
	$(eval $(call SPARCV9_USER_BASIC_COMMAND,$(command))))
basic-tools: $(USER_BASIC_TARGETS)
.PHONY: basic-tools

$(BUILD)/POSIX-R1.ELF: $(BUILD)/user/src/crt/crt0-sparcv9.o \
	$(SPARCV9_USER_RUNTIME_OBJS) \
	$(BUILD)/user/userland/base/tests/syscall-smoke.o \
	$(SPARCV9_PLATFORM)/user.ld tools/build/check-user-elf.py
	$(SPARCV9_CC) $(SPARCV9_USER_CFLAGS) -nostdlib -static \
		-Wl,--gc-sections -Wl,-z,max-page-size=8192 \
		-Wl,-T,$(SPARCV9_PLATFORM)/user.ld \
		$(BUILD)/user/src/crt/crt0-sparcv9.o \
		$(SPARCV9_USER_RUNTIME_OBJS) \
		$(BUILD)/user/userland/base/tests/syscall-smoke.o -lgcc -o $@
	@test -z "$$($(SPARCV9_NM) -u $@)" || { $(SPARCV9_NM) -u $@; exit 1; }
	$(PYTHON) tools/build/check-user-elf.py --machine sparcv9 $@

$(BUILD)/POSIX-R2.ELF: $(BUILD)/user/src/crt/crt0-sparcv9.o \
	$(SPARCV9_USER_RUNTIME_OBJS) \
	$(BUILD)/user/userland/base/tests/posix-r2.o \
	$(SPARCV9_PLATFORM)/user.ld tools/build/check-user-elf.py
	$(SPARCV9_CC) $(SPARCV9_USER_CFLAGS) -nostdlib -static \
		-Wl,--gc-sections -Wl,-z,max-page-size=8192 \
		-Wl,-T,$(SPARCV9_PLATFORM)/user.ld \
		$(BUILD)/user/src/crt/crt0-sparcv9.o \
		$(SPARCV9_USER_RUNTIME_OBJS) \
		$(BUILD)/user/userland/base/tests/posix-r2.o -lgcc -o $@
	@test -z "$$($(SPARCV9_NM) -u $@)" || { $(SPARCV9_NM) -u $@; exit 1; }
	$(PYTHON) tools/build/check-user-elf.py --machine sparcv9 $@

$(BUILD)/POSIX-R2-REMAINING.ELF: \
	$(BUILD)/user/src/crt/crt0-sparcv9.o $(SPARCV9_USER_RUNTIME_OBJS) \
	$(BUILD)/user/userland/base/tests/posix-r2-remaining.o \
	$(SPARCV9_PLATFORM)/user.ld tools/build/check-user-elf.py
	$(SPARCV9_CC) $(SPARCV9_USER_CFLAGS) -nostdlib -static \
		-Wl,--gc-sections -Wl,-z,max-page-size=8192 \
		-Wl,-T,$(SPARCV9_PLATFORM)/user.ld \
		$(BUILD)/user/src/crt/crt0-sparcv9.o \
		$(SPARCV9_USER_RUNTIME_OBJS) \
		$(BUILD)/user/userland/base/tests/posix-r2-remaining.o -lgcc -o $@
	@test -z "$$($(SPARCV9_NM) -u $@)" || { $(SPARCV9_NM) -u $@; exit 1; }
	$(PYTHON) tools/build/check-user-elf.py --machine sparcv9 $@

# ELF64 runtime linker and shared libc for the SPARC V9 userland.
SPARCV9_DYNAMIC_DIR := $(BUILD)/dynamic
SPARCV9_DYNAMIC_CPPFLAGS := -nostdinc -I. -Iinclude -Iinclude/uapi \
	-Ilibc/include -DHAL_ARCH_SPARCV9 -DZEDBSD_USER_ABI_SPARCV9 \
	-DZEDBSD_USER_ABI_LP64 -DZEDBSD_USER_PAGE_SIZE=8192 \
	-DZEDBSD_DYNAMIC_LIBC
SPARCV9_DYNAMIC_CFLAGS := -m64 -mcpu=ultrasparc -mstack-bias \
	-mcmodel=medany -msoft-float -mno-app-regs -Os -ffreestanding -fPIC \
	-fno-builtin -fno-stack-protector -fno-asynchronous-unwind-tables \
	-fno-unwind-tables -fno-plt -ftls-model=global-dynamic \
	-Wall -Wextra -Werro
SPARCV9_DYNAMIC_LIBC_SOURCES := userland/base/libc/posix.c \
	userland/base/libc/poll.c userland/base/libc/termios.c userland/base/libc/pthread.c \
	userland/base/libc/shm.c userland/base/libc/semaphore.c userland/base/libc/mqueue.c \
	userland/base/libc/dlfcn.c \
	userland/base/libc/socket.c userland/base/libc/signal.c \
	libc/heap.c libc/string.c libc/ctype.c libc/locale.c libc/wide.c libc/int64.c libc/strto.c \
	libc/format.c libc/stdio.c
SPARCV9_DYNAMIC_LIBC_OBJS := $(patsubst %.c,$(SPARCV9_DYNAMIC_DIR)/obj/%.o,\
	$(SPARCV9_DYNAMIC_LIBC_SOURCES)) \
	$(SPARCV9_DYNAMIC_DIR)/obj/userland/base/libc/syscall.o \
	$(SPARCV9_DYNAMIC_DIR)/obj/src/softfloat/sparcv9/libgcc-runtime.o

# The bare-metal SPARC toolchain does not provide the soft-float arithmetic
# entry points in libgcc.a.  Build the exact GCC soft-fp operations required by
# libc, including the 128-bit long-double helpers used by musl's scanner.
SPARCV9_DYNAMIC_SOFTFP_REL := $(ZEDBSD_GCC_SOFTFP_REL) \
	addtf3.c divtf3.c eqtf2.c extenddftf2.c extendsftf2.c \
	floatsitf.c floatunsitf.c getf2.c letf2.c multf3.c subtf3.c \
	trunctfdf2.c trunctfsf2.c unordtf2.c
SPARCV9_DYNAMIC_SOFTFP_OBJS := $(addprefix \
	$(SPARCV9_DYNAMIC_DIR)/softfp/gcc-,\
	$(SPARCV9_DYNAMIC_SOFTFP_REL:.c=.o))
SPARCV9_DYNAMIC_RTLD_OBJS := \
	$(SPARCV9_DYNAMIC_DIR)/obj/userland/base/rtld/entry.o \
	$(SPARCV9_DYNAMIC_DIR)/obj/userland/base/rtld/rtld.o \
	$(SPARCV9_DYNAMIC_DIR)/obj/userland/base/rtld/string.o
SPARCV9_DYNAMIC_FLOAT_DIR := $(SPARCV9_DYNAMIC_DIR)/float
SPARCV9_DYNAMIC_MUSL_MATH_OBJS := $(addprefix \
	$(SPARCV9_DYNAMIC_FLOAT_DIR)/musl-,$(ZEDBSD_MUSL_MATH_REL:.c=.o))
SPARCV9_DYNAMIC_MUSL_SCAN_OBJS := \
	$(SPARCV9_DYNAMIC_FLOAT_DIR)/musl-shgetc.o \
	$(SPARCV9_DYNAMIC_FLOAT_DIR)/musl-floatscan.o \
	$(SPARCV9_DYNAMIC_FLOAT_DIR)/musl-strtod.o \
	$(SPARCV9_DYNAMIC_FLOAT_DIR)/musl-compat.o
SPARCV9_DYNAMIC_LIBC_OBJS += $(SPARCV9_DYNAMIC_MUSL_MATH_OBJS) \
	$(SPARCV9_DYNAMIC_MUSL_SCAN_OBJS) $(SPARCV9_DYNAMIC_SOFTFP_OBJS)

$(SPARCV9_DYNAMIC_DIR)/softfp/gcc-%.o: \
	$(ZEDBSD_GCC_ROOT)/libgcc/soft-fp/%.c
	@mkdir -p $(dir $@)
	$(SPARCV9_CC) -nostdinc -Ilibc/include -I. \
		-Isrc/softfloat/sparcv9 -I$(ZEDBSD_GCC_ROOT)/include \
		-I$(ZEDBSD_GCC_ROOT)/libgcc \
		-I$(ZEDBSD_GCC_ROOT)/libgcc/soft-fp -D_SOFT_FLOAT \
		$(SPARCV9_DYNAMIC_CFLAGS) -Wno-error=type-limits \
		-MMD -MP -c $< -o $@

$(SPARCV9_DYNAMIC_DIR)/obj/%.o: %.c
	@mkdir -p $(dir $@)
	$(SPARCV9_CC) $(SPARCV9_DYNAMIC_CPPFLAGS) \
		$(SPARCV9_DYNAMIC_CFLAGS) -MMD -MP -c $< -o $@

$(SPARCV9_DYNAMIC_DIR)/obj/userland/base/libc/syscall.o: \
	userland/base/libc/syscall-sparcv9.S
	@mkdir -p $(dir $@)
	$(SPARCV9_CC) $(SPARCV9_DYNAMIC_CFLAGS) -c $< -o $@

$(SPARCV9_DYNAMIC_DIR)/obj/userland/base/rtld/entry.o: \
	userland/base/rtld/entry-sparcv9.S
	@mkdir -p $(dir $@)
	$(SPARCV9_CC) $(SPARCV9_DYNAMIC_CFLAGS) -c $< -o $@

$(SPARCV9_DYNAMIC_DIR)/obj/src/crt/crt1.o: src/crt/crt1-sparcv9.S
	@mkdir -p $(dir $@)
	$(SPARCV9_CC) $(SPARCV9_DYNAMIC_CFLAGS) -c $< -o $@

$(SPARCV9_DYNAMIC_FLOAT_DIR)/musl-%.o: \
	$(ZEDBSD_MUSL_ROOT)/src/math/%.c
	@mkdir -p $(dir $@)
	$(SPARCV9_CC) $(ZEDBSD_MUSL_CPPFLAGS) \
		$(SPARCV9_DYNAMIC_CFLAGS) -Wno-error=unused-but-set-variable \
		-Wno-error=parentheses -c $< -o $@

$(SPARCV9_DYNAMIC_FLOAT_DIR)/musl-shgetc.o: \
	$(ZEDBSD_MUSL_ROOT)/src/internal/shgetc.c src/softfloat/musl-floatscan.h
	@mkdir -p $(dir $@)
	$(SPARCV9_CC) $(ZEDBSD_MUSL_CPPFLAGS) \
		$(SPARCV9_DYNAMIC_CFLAGS) -Wno-error=parentheses \
		-include src/softfloat/musl-floatscan.h -c $< -o $@

$(SPARCV9_DYNAMIC_FLOAT_DIR)/musl-floatscan.o: \
	$(ZEDBSD_MUSL_ROOT)/src/internal/floatscan.c src/softfloat/musl-floatscan.h
	@mkdir -p $(dir $@)
	$(SPARCV9_CC) $(ZEDBSD_MUSL_CPPFLAGS) \
		$(SPARCV9_DYNAMIC_CFLAGS) -Wno-error=parentheses \
		-Wno-error=sign-compare -include src/softfloat/musl-floatscan.h \
		-c $< -o $@

$(SPARCV9_DYNAMIC_FLOAT_DIR)/musl-strtod.o: \
	$(ZEDBSD_MUSL_ROOT)/src/stdlib/strtod.c src/softfloat/musl-floatscan.h
	@mkdir -p $(dir $@)
	$(SPARCV9_CC) $(ZEDBSD_MUSL_CPPFLAGS) \
		$(SPARCV9_DYNAMIC_CFLAGS) -include src/softfloat/musl-floatscan.h \
		-c $< -o $@

$(SPARCV9_DYNAMIC_FLOAT_DIR)/musl-compat.o: \
	src/softfloat/musl-compat.c src/softfloat/musl-floatscan.h
	@mkdir -p $(dir $@)
	$(SPARCV9_CC) $(ZEDBSD_MUSL_CPPFLAGS) \
		$(SPARCV9_DYNAMIC_CFLAGS) -include src/softfloat/musl-floatscan.h \
		-c $< -o $@

$(SPARCV9_DYNAMIC_DIR)/ld.so: $(SPARCV9_DYNAMIC_RTLD_OBJS)
	$(SPARCV9_LD) -m elf64_sparc -shared -Bsymbolic -e _rtld_start \
		--hash-style=sysv -z now -z relro -z separate-code \
		-z max-page-size=8192 $^ -o $@

$(SPARCV9_DYNAMIC_DIR)/libc.so: $(SPARCV9_DYNAMIC_LIBC_OBJS)
	$(SPARCV9_LD) -m elf64_sparc -shared -Bsymbolic-functions \
		-T $(SPARCV9_PLATFORM)/dynamic-plt.ld \
		-z undefs -z noexecstack \
		-soname libc.so --hash-style=both -z now -z relro \
		-z separate-code -z max-page-size=8192 -z stack-size=0x100000 \
		$^ -o $@

$(SPARCV9_DYNAMIC_DIR)/alt/rpathdep.so: \
	$(SPARCV9_DYNAMIC_DIR)/obj/userland/base/tests/rpathdep.o \
	$(SPARCV9_DYNAMIC_DIR)/ld.so
	@mkdir -p $(dir $@)
	$(SPARCV9_LD) -m elf64_sparc -shared -Bsymbolic-functions \
		-T $(SPARCV9_PLATFORM)/dynamic-plt.ld \
		-soname rpathdep.so --hash-style=gnu -z now -z relro \
		-z separate-code -z max-page-size=8192 $(filter %.o,$^) -o $@

$(SPARCV9_DYNAMIC_DIR)/tlstest.so: \
	$(SPARCV9_DYNAMIC_DIR)/obj/userland/base/tests/tlstest.o \
	$(SPARCV9_DYNAMIC_DIR)/alt/rpathdep.so \
	$(SPARCV9_DYNAMIC_DIR)/ld.so
	$(SPARCV9_LD) -m elf64_sparc -shared -Bsymbolic-functions \
		-T $(SPARCV9_PLATFORM)/dynamic-plt.ld \
		-soname tlstest.so \
		--hash-style=gnu -z now -z relro -z separate-code \
		-z max-page-size=8192 --enable-new-dtags -rpath '$$ORIGIN/alt' \
		$(SPARCV9_DYNAMIC_DIR)/obj/userland/base/tests/tlstest.o \
		-L$(SPARCV9_DYNAMIC_DIR)/alt -l:rpathdep.so -o $@

$(SPARCV9_DYNAMIC_DIR)/rpathtest.so: \
	$(SPARCV9_DYNAMIC_DIR)/obj/userland/base/tests/rpathtest.o \
	$(SPARCV9_DYNAMIC_DIR)/alt/rpathdep.so \
	$(SPARCV9_DYNAMIC_DIR)/ld.so
	$(SPARCV9_LD) -m elf64_sparc -shared -Bsymbolic-functions \
		-T $(SPARCV9_PLATFORM)/dynamic-plt.ld -soname rpthtest.so \
		--hash-style=gnu -z now -z relro -z separate-code \
		-z max-page-size=8192 --disable-new-dtags -rpath '$$ORIGIN/alt' \
		$(SPARCV9_DYNAMIC_DIR)/obj/userland/base/tests/rpathtest.o \
		-L$(SPARCV9_DYNAMIC_DIR)/alt -l:rpathdep.so -o $@

$(SPARCV9_DYNAMIC_DIR)/verstest.so: \
	$(SPARCV9_DYNAMIC_DIR)/obj/userland/base/tests/versiontest.o \
	userland/base/tests/versiontest.map $(SPARCV9_DYNAMIC_DIR)/ld.so
	$(SPARCV9_LD) -m elf64_sparc -shared -Bsymbolic-functions \
		-T $(SPARCV9_PLATFORM)/dynamic-plt.ld -soname verstest.so \
		--hash-style=gnu -z now -z relro -z separate-code \
		-z max-page-size=8192 \
		--version-script=userland/base/tests/versiontest.map \
		$(SPARCV9_DYNAMIC_DIR)/obj/userland/base/tests/versiontest.o -o $@

$(SPARCV9_DYNAMIC_DIR)/versuse.so: \
	$(SPARCV9_DYNAMIC_DIR)/obj/userland/base/tests/versionuse.o \
	$(SPARCV9_DYNAMIC_DIR)/verstest.so $(SPARCV9_DYNAMIC_DIR)/ld.so
	$(SPARCV9_LD) -m elf64_sparc -shared -Bsymbolic-functions \
		-T $(SPARCV9_PLATFORM)/dynamic-plt.ld -soname versuse.so \
		--hash-style=gnu -z now -z relro -z separate-code \
		-z max-page-size=8192 \
		$(SPARCV9_DYNAMIC_DIR)/obj/userland/base/tests/versionuse.o \
		-L$(SPARCV9_DYNAMIC_DIR) -l:verstest.so -o $@

$(SPARCV9_DYNAMIC_DIR)/dyntest: \
	$(SPARCV9_DYNAMIC_DIR)/obj/src/crt/crt1.o \
	$(SPARCV9_DYNAMIC_DIR)/obj/userland/base/tests/dyntest.o \
	$(SPARCV9_DYNAMIC_DIR)/libc.so $(SPARCV9_DYNAMIC_DIR)/ld.so \
	$(SPARCV9_DYNAMIC_DIR)/tlstest.so $(SPARCV9_DYNAMIC_DIR)/versuse.so
	$(SPARCV9_LD) -m elf64_sparc -pie -e _start --no-relax \
		-T $(SPARCV9_PLATFORM)/dynamic-plt.ld \
		--hash-style=sysv -z now -z relro -z noexecstack \
		-z separate-code -z max-page-size=8192 -z stack-size=0x100000 \
		--allow-shlib-undefined --dynamic-linker=/lib/ld.so \
		$(SPARCV9_DYNAMIC_DIR)/obj/src/crt/crt1.o \
		$(SPARCV9_DYNAMIC_DIR)/obj/userland/base/tests/dyntest.o \
		-L$(SPARCV9_DYNAMIC_DIR) -rpath-link $(SPARCV9_DYNAMIC_DIR) \
		-l:libc.so -o $@

dynamic-userland-check: $(SPARCV9_DYNAMIC_DIR)/ld.so \
	$(SPARCV9_DYNAMIC_DIR)/libc.so $(SPARCV9_DYNAMIC_DIR)/dyntest \
	$(SPARCV9_DYNAMIC_DIR)/tlstest.so \
	$(SPARCV9_DYNAMIC_DIR)/rpathtest.so \
	$(SPARCV9_DYNAMIC_DIR)/verstest.so \
	$(SPARCV9_DYNAMIC_DIR)/versuse.so tools/build/check-dynamic-elf.py
	$(PYTHON) tools/build/check-dynamic-elf.py --machine sparcv9 \
		--role interpreter $(SPARCV9_DYNAMIC_DIR)/ld.so
	$(PYTHON) tools/build/check-dynamic-elf.py --machine sparcv9 \
		--role libc $(SPARCV9_DYNAMIC_DIR)/libc.so
	$(PYTHON) tools/build/check-dynamic-elf.py --machine sparcv9 \
		--role module $(SPARCV9_DYNAMIC_DIR)/tlstest.so
	$(PYTHON) tools/build/check-dynamic-elf.py --machine sparcv9 \
		--role rpath-module $(SPARCV9_DYNAMIC_DIR)/rpathtest.so
	$(PYTHON) tools/build/check-dynamic-elf.py --machine sparcv9 \
		--role version-definition $(SPARCV9_DYNAMIC_DIR)/verstest.so
	$(PYTHON) tools/build/check-dynamic-elf.py --machine sparcv9 \
		--role version-consumer $(SPARCV9_DYNAMIC_DIR)/versuse.so
	$(PYTHON) tools/build/check-dynamic-elf.py --machine sparcv9 \
		--role program $(SPARCV9_DYNAMIC_DIR)/dyntest
	@echo "zedBSD SPARC V9 dynamic userland artifacts: PASS"

.PHONY: dynamic-userland-check

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
	$(PYTHON) tools/build/check-sparcv9-boot.py \
		--stage1 $(BUILD)/boot/stage1.bin \
		--stage2 $(BUILD)/boot/stage2.bin

SPARCV9_ROOTFS_INPUTS := $(BUILD)/bin/sh $(BUILD)/bin/sysctl \
	$(SPARCV9_DYNAMIC_DIR)/ld.so $(SPARCV9_DYNAMIC_DIR)/libc.so \
	$(SPARCV9_DYNAMIC_DIR)/tlstest.so $(SPARCV9_DYNAMIC_DIR)/dyntest \
	$(SPARCV9_DYNAMIC_DIR)/alt/rpathdep.so \
	$(SPARCV9_DYNAMIC_DIR)/rpathtest.so \
	$(SPARCV9_DYNAMIC_DIR)/verstest.so \
	$(SPARCV9_DYNAMIC_DIR)/versuse.so
SPARCV9_ROOTFS_FILES := --file /bin/sh=$(BUILD)/bin/sh \
	--file /bin/sysctl=$(BUILD)/bin/sysctl \
	--file /lib/ld.so=$(SPARCV9_DYNAMIC_DIR)/ld.so \
	--file /lib/libc.so=$(SPARCV9_DYNAMIC_DIR)/libc.so \
	--file /lib/tlstest.so=$(SPARCV9_DYNAMIC_DIR)/tlstest.so \
	--file /bin/dyntest=$(SPARCV9_DYNAMIC_DIR)/dyntest \
	--file /lib/alt/rpathdep.so=$(SPARCV9_DYNAMIC_DIR)/alt/rpathdep.so \
	--file /lib/rpthtest.so=$(SPARCV9_DYNAMIC_DIR)/rpathtest.so \
	--file /lib/verstest.so=$(SPARCV9_DYNAMIC_DIR)/verstest.so \
	--file /lib/versuse.so=$(SPARCV9_DYNAMIC_DIR)/versuse.so
$(eval $(call ZEDBSD_ROOTFS_TAR_RULE,$(BUILD)/rootfs.tar.gz,$(SPARCV9_ROOTFS_INPUTS),$(SPARCV9_ROOTFS_FILES)))

rootfs-tar: $(BUILD)/rootfs.tar.gz
rootfs: $(BUILD)/rootfs/.stamp

$(BUILD)/hdd-image.img: $(BUILD)/vmunix $(BUILD)/bin/sh $(BUILD)/bin/sysctl \
	$(SPARCV9_DYNAMIC_DIR)/ld.so $(SPARCV9_DYNAMIC_DIR)/libc.so \
	$(SPARCV9_DYNAMIC_DIR)/tlstest.so $(SPARCV9_DYNAMIC_DIR)/dyntest \
	$(SPARCV9_DYNAMIC_DIR)/alt/rpathdep.so \
	$(SPARCV9_DYNAMIC_DIR)/rpathtest.so \
	$(SPARCV9_DYNAMIC_DIR)/verstest.so \
	$(SPARCV9_DYNAMIC_DIR)/versuse.so \
	$(BUILD)/boot/stage1.bin \
	$(BUILD)/boot/stage2.bin tools/build/make-sparcv9-hdd-image.py \
	tools/build/check-sparcv9-hdd-image.py
	$(PYTHON) tools/build/make-sparcv9-hdd-image.py --force \
		--stage1 $(BUILD)/boot/stage1.bin \
		--stage2 $(BUILD)/boot/stage2.bin \
		--kernel $(BUILD)/vmunix --shell $(BUILD)/bin/sh \
		--sysctl $(BUILD)/bin/sysctl \
		--rtld $(SPARCV9_DYNAMIC_DIR)/ld.so \
		--libc $(SPARCV9_DYNAMIC_DIR)/libc.so \
		--tlstest $(SPARCV9_DYNAMIC_DIR)/tlstest.so \
		--rpathdep $(SPARCV9_DYNAMIC_DIR)/alt/rpathdep.so \
		--rpathtest $(SPARCV9_DYNAMIC_DIR)/rpathtest.so \
		--verstest $(SPARCV9_DYNAMIC_DIR)/verstest.so \
		--versuse $(SPARCV9_DYNAMIC_DIR)/versuse.so \
		--dyntest $(SPARCV9_DYNAMIC_DIR)/dyntest $@

$(BUILD)/ufs-root.img: $(BUILD)/bin/sh $(BUILD)/bin/sysctl \
	$(SPARCV9_DYNAMIC_DIR)/ld.so $(SPARCV9_DYNAMIC_DIR)/libc.so \
	$(SPARCV9_DYNAMIC_DIR)/tlstest.so $(SPARCV9_DYNAMIC_DIR)/dyntest \
	$(SPARCV9_DYNAMIC_DIR)/alt/rpathdep.so \
	$(SPARCV9_DYNAMIC_DIR)/rpathtest.so \
	$(SPARCV9_DYNAMIC_DIR)/verstest.so \
	$(SPARCV9_DYNAMIC_DIR)/versuse.so \
	tools/build/make-ufs1-root-image.py \
	tools/build/ufs1_format.py
	$(PYTHON) tools/build/make-ufs1-root-image.py --force \
		--arch-profile sparcv9 --native-shell $(BUILD)/bin/sh \
		--native-sysctl $(BUILD)/bin/sysctl \
		--native-rtld $(SPARCV9_DYNAMIC_DIR)/ld.so \
		--native-libc $(SPARCV9_DYNAMIC_DIR)/libc.so \
		--native-tlstest $(SPARCV9_DYNAMIC_DIR)/tlstest.so \
		--native-rpathdep $(SPARCV9_DYNAMIC_DIR)/alt/rpathdep.so \
		--native-rpathtest $(SPARCV9_DYNAMIC_DIR)/rpathtest.so \
		--native-verstest $(SPARCV9_DYNAMIC_DIR)/verstest.so \
		--native-versuse $(SPARCV9_DYNAMIC_DIR)/versuse.so \
		--native-dyntest $(SPARCV9_DYNAMIC_DIR)/dyntest $@

$(BUILD)/ufs-root-hdd-image.img: $(BUILD)/vmunix $(BUILD)/bin/sh \
	$(BUILD)/bin/sysctl \
	$(BUILD)/boot/stage1.bin $(BUILD)/boot/stage2.bin \
	$(BUILD)/ufs-root.img tools/build/make-sparcv9-hdd-image.py \
	tools/build/check-sparcv9-hdd-image.py tools/build/check-ufs1-image.py
	$(PYTHON) tools/build/make-sparcv9-hdd-image.py --force \
		--stage1 $(BUILD)/boot/stage1.bin \
		--stage2 $(BUILD)/boot/stage2.bin \
		--kernel $(BUILD)/vmunix --shell $(BUILD)/bin/sh \
		--sysctl $(BUILD)/bin/sysctl \
		--ufs-root $(BUILD)/ufs-root.img $@

ufs-root-image: $(BUILD)/ufs-root-hdd-image.img

ROOTFS_IMAGE_BUILD_TARGET := $(BUILD)/ufs-root.img
ROOTFS_IMAGE_ARTIFACT := $(BUILD)/ufs-root.img

hdd-image: $(BUILD)/hdd-image.img

sparcv9-disk-check: $(BUILD)/hdd-image.img
	$(PYTHON) tools/build/check-sparcv9-hdd-image.py \
		--stage1 $(BUILD)/boot/stage1.bin \
		--stage2 $(BUILD)/boot/stage2.bin \
		--kernel $(BUILD)/vmunix --shell $(BUILD)/bin/sh \
		--sysctl $(BUILD)/bin/sysctl \
		--rtld $(SPARCV9_DYNAMIC_DIR)/ld.so \
		--libc $(SPARCV9_DYNAMIC_DIR)/libc.so \
		--tlstest $(SPARCV9_DYNAMIC_DIR)/tlstest.so \
		--rpathdep $(SPARCV9_DYNAMIC_DIR)/alt/rpathdep.so \
		--rpathtest $(SPARCV9_DYNAMIC_DIR)/rpathtest.so \
		--verstest $(SPARCV9_DYNAMIC_DIR)/verstest.so \
		--versuse $(SPARCV9_DYNAMIC_DIR)/versuse.so \
		--dyntest $(SPARCV9_DYNAMIC_DIR)/dyntest $<

sparcv9-entry-qemu-test: $(BUILD)/hdd-image.img
	bash scripts/test-sparcv9-entry-qemu.sh

sparcv9-qemu-test: $(BUILD)/hdd-image.img
	bash scripts/test-sparcv9-qemu.sh

sparcv9-ufs-qemu-test: $(BUILD)/ufs-root-hdd-image.img
	bash scripts/test-sparcv9-ufs-qemu.sh

.PHONY: all vmunix SH POSIX-R1.ELF POSIX-R2.ELF POSIX-R2-REMAINING.ELF hdd-image rootfs-tar ufs-root-image sparcv9-toolchain sparcv9-image-check \
	sparcv9-bootloader sparcv9-disk-check sparcv9-entry-qemu-test \
	sparcv9-qemu-test sparcv9-ufs-qemu-test

-include $(SPARCV9_EARLY_OBJS:.o=.d) \
	$(SPARCV9_STAGE1_OBJS:.o=.d) $(SPARCV9_STAGE2_OBJS:.o=.d)
-include $(SPARCV9_KERNEL_OBJS:.o=.d) $(SPARCV9_KERNEL_LIBC_OBJS:.o=.d)
-include $(SPARCV9_USER_OBJS:.o=.d)

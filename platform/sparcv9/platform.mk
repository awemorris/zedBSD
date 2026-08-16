# zedBSD SPARC V9/sun4u bootstrap rules.
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib

SPARCV9_PREFIX ?= $(HOME)/opt/sparcv9
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
	src/kern/mount.c src/kern/rootfs.c src/kern/tmpfs.c src/kern/overlayfs.c \
	src/kern/vfs.c src/kern/swap.c \
	src/kern/swap-fat.c src/kern/vm-reclaim.c src/kern/buf.c \
	src/kern/sysctl.c src/kern/resource.c src/kern/poll.c src/kern/usync.c src/kern/disk.c \
	src/kern/resource-limit.c \
	drivers/loop.c \
	src/kern/partition.c src/kern/sun-disklabel.c src/kern/sun4u/platform.c \
	drivers/sun4u-cmd646.c src/kern/image.c src/kern/panic.c \
	src/kern/entry.c src/kern/clock.c src/kern/lock.c src/kern/waitq.c \
	src/kern/process.c src/kern/thread.c \
	src/kern/sched.c src/kern/vmspace.c src/kern/vm-object.c \
	src/kern/vm-commit.c src/kern/filedesc.c src/kern/pipe.c \
	src/kern/record-lock.c \
	src/kern/cred.c src/kern/signal.c src/kern/cwdinfo.c \
	src/kern/elf.c src/kern/exec.c \
	src/kern/user-probe.c src/kern/syscall.c src/kern/uaccess.c \
	src/kern/cdev.c src/kern/devfs.c src/kern/console-device.c src/kern/tty.c \
	src/kern/graphics-device.c src/kern/system-device.c \
	src/kern/init.c
SPARCV9_KERNEL_SOURCES += $(KERN_NET_SOURCES) $(KERN_UFS1_SOURCES)
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
SPARCV9_USER_RUNTIME_SOURCES := userland/libc/posix.c userland/libc/dlfcn.c userland/libc/static-tls.c userland/libc/poll.c \
	userland/libc/termios.c \
	userland/libc/pthread.c \
	userland/libc/shm.c \
	userland/libc/semaphore.c \
	userland/libc/mqueue.c \
	userland/libc/socket.c \
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

all: $(BUILD)/vmunix $(BUILD)/bin/sh $(BUILD)/bin/sysctl \
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

SPARCV9_USER_SYSCTL_OBJ := $(BUILD)/user/userland/sysctl/main.o
$(BUILD)/bin/sysctl: $(BUILD)/user/userland/crt0-sparcv9.o \
	$(SPARCV9_USER_RUNTIME_OBJS) $(SPARCV9_USER_SYSCTL_OBJ) \
	$(SPARCV9_PLATFORM)/user.ld scripts/check-user-elf.py
	@mkdir -p $(dir $@)
	$(SPARCV9_CC) $(SPARCV9_USER_CFLAGS) -nostdlib -static \
		-Wl,--gc-sections -Wl,-z,max-page-size=8192 \
		-Wl,-T,$(SPARCV9_PLATFORM)/user.ld \
		$(BUILD)/user/userland/crt0-sparcv9.o \
		$(SPARCV9_USER_RUNTIME_OBJS) $(SPARCV9_USER_SYSCTL_OBJ) \
		-lgcc -o $@
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

$(BUILD)/POSIX-R2.ELF: $(BUILD)/user/userland/crt0-sparcv9.o \
	$(SPARCV9_USER_RUNTIME_OBJS) \
	$(BUILD)/user/userland/tests/posix-r2.o \
	$(SPARCV9_PLATFORM)/user.ld scripts/check-user-elf.py
	$(SPARCV9_CC) $(SPARCV9_USER_CFLAGS) -nostdlib -static \
		-Wl,--gc-sections -Wl,-z,max-page-size=8192 \
		-Wl,-T,$(SPARCV9_PLATFORM)/user.ld \
		$(BUILD)/user/userland/crt0-sparcv9.o \
		$(SPARCV9_USER_RUNTIME_OBJS) \
		$(BUILD)/user/userland/tests/posix-r2.o -lgcc -o $@
	@test -z "$$($(SPARCV9_NM) -u $@)" || { $(SPARCV9_NM) -u $@; exit 1; }
	$(PYTHON) scripts/check-user-elf.py --machine sparcv9 $@

$(BUILD)/POSIX-R2-REMAINING.ELF: \
	$(BUILD)/user/userland/crt0-sparcv9.o $(SPARCV9_USER_RUNTIME_OBJS) \
	$(BUILD)/user/userland/tests/posix-r2-remaining.o \
	$(SPARCV9_PLATFORM)/user.ld scripts/check-user-elf.py
	$(SPARCV9_CC) $(SPARCV9_USER_CFLAGS) -nostdlib -static \
		-Wl,--gc-sections -Wl,-z,max-page-size=8192 \
		-Wl,-T,$(SPARCV9_PLATFORM)/user.ld \
		$(BUILD)/user/userland/crt0-sparcv9.o \
		$(SPARCV9_USER_RUNTIME_OBJS) \
		$(BUILD)/user/userland/tests/posix-r2-remaining.o -lgcc -o $@
	@test -z "$$($(SPARCV9_NM) -u $@)" || { $(SPARCV9_NM) -u $@; exit 1; }
	$(PYTHON) scripts/check-user-elf.py --machine sparcv9 $@

# ELF64 runtime linker and shared libc for the SPARC V9 userland.
SPARCV9_DYNAMIC_DIR := $(BUILD)/dynamic
SPARCV9_DYNAMIC_CPPFLAGS := -nostdinc -I. -Iinclude -Iinclude/uapi \
	-Ilibc/include -DHAL_ARCH_SPARCV9 -DZEDBSD_USER_ABI_SPARCV9 \
	-DZEDBSD_USER_ABI_LP64 -DZEDBSD_USER_PAGE_SIZE=8192 \
	-DZEDBSD_DYNAMIC_LIBC
SPARCV9_DYNAMIC_CFLAGS := -m64 -mcpu=ultrasparc -mstack-bias \
	-mcmodel=medany -msoft-float -mno-app-regs -Os -ffreestanding -fPIC \
	-fno-builtin -fno-stack-protector -fno-asynchronous-unwind-tables \
	-fno-unwind-tables -ftls-model=global-dynamic -Wall -Wextra -Werror
SPARCV9_DYNAMIC_LIBC_SOURCES := userland/libc/posix.c \
	userland/libc/poll.c userland/libc/termios.c userland/libc/pthread.c \
	userland/libc/shm.c userland/libc/semaphore.c userland/libc/mqueue.c \
	userland/libc/dlfcn.c userland/libc/socket.c userland/libc/signal.c \
	libc/heap.c libc/string.c libc/ctype.c libc/int64.c libc/strto.c \
	libc/format.c libc/stdio.c
SPARCV9_DYNAMIC_LIBC_OBJS := $(patsubst %.c,$(SPARCV9_DYNAMIC_DIR)/obj/%.o,\
	$(SPARCV9_DYNAMIC_LIBC_SOURCES)) \
	$(SPARCV9_DYNAMIC_DIR)/obj/userland/libc/syscall.o
SPARCV9_DYNAMIC_RTLD_OBJS := \
	$(SPARCV9_DYNAMIC_DIR)/obj/userland/rtld/entry.o \
	$(SPARCV9_DYNAMIC_DIR)/obj/userland/rtld/rtld.o \
	$(SPARCV9_DYNAMIC_DIR)/obj/userland/rtld/string.o
SPARCV9_DYNAMIC_FLOAT_DIR := $(SPARCV9_DYNAMIC_DIR)/float
SPARCV9_DYNAMIC_MUSL_MATH_OBJS := $(addprefix \
	$(SPARCV9_DYNAMIC_FLOAT_DIR)/musl-,$(ZEDBSD_MUSL_MATH_REL:.c=.o))
SPARCV9_DYNAMIC_MUSL_SCAN_OBJS := \
	$(SPARCV9_DYNAMIC_FLOAT_DIR)/musl-shgetc.o \
	$(SPARCV9_DYNAMIC_FLOAT_DIR)/musl-floatscan.o \
	$(SPARCV9_DYNAMIC_FLOAT_DIR)/musl-strtod.o \
	$(SPARCV9_DYNAMIC_FLOAT_DIR)/musl-compat.o
SPARCV9_DYNAMIC_LIBC_OBJS += $(SPARCV9_DYNAMIC_MUSL_MATH_OBJS) \
	$(SPARCV9_DYNAMIC_MUSL_SCAN_OBJS)

$(SPARCV9_DYNAMIC_DIR)/obj/%.o: %.c
	@mkdir -p $(dir $@)
	$(SPARCV9_CC) $(SPARCV9_DYNAMIC_CPPFLAGS) \
		$(SPARCV9_DYNAMIC_CFLAGS) -MMD -MP -c $< -o $@

$(SPARCV9_DYNAMIC_DIR)/obj/userland/libc/syscall.o: \
	userland/libc/syscall-sparcv9.S
	@mkdir -p $(dir $@)
	$(SPARCV9_CC) $(SPARCV9_DYNAMIC_CFLAGS) -c $< -o $@

$(SPARCV9_DYNAMIC_DIR)/obj/userland/rtld/entry.o: \
	userland/rtld/entry-sparcv9.S
	@mkdir -p $(dir $@)
	$(SPARCV9_CC) $(SPARCV9_DYNAMIC_CFLAGS) -c $< -o $@

$(SPARCV9_DYNAMIC_DIR)/obj/userland/crt1.o: userland/crt1-sparcv9.S
	@mkdir -p $(dir $@)
	$(SPARCV9_CC) $(SPARCV9_DYNAMIC_CFLAGS) -c $< -o $@

$(SPARCV9_DYNAMIC_FLOAT_DIR)/musl-%.o: \
	$(ZEDBSD_MUSL_ROOT)/src/math/%.c
	@mkdir -p $(dir $@)
	$(SPARCV9_CC) $(ZEDBSD_MUSL_CPPFLAGS) \
		$(SPARCV9_DYNAMIC_CFLAGS) -Wno-error=unused-but-set-variable \
		-Wno-error=parentheses -c $< -o $@

$(SPARCV9_DYNAMIC_FLOAT_DIR)/musl-shgetc.o: \
	$(ZEDBSD_MUSL_ROOT)/src/internal/shgetc.c softfloat/musl-floatscan.h
	@mkdir -p $(dir $@)
	$(SPARCV9_CC) $(ZEDBSD_MUSL_CPPFLAGS) \
		$(SPARCV9_DYNAMIC_CFLAGS) -Wno-error=parentheses \
		-include softfloat/musl-floatscan.h -c $< -o $@

$(SPARCV9_DYNAMIC_FLOAT_DIR)/musl-floatscan.o: \
	$(ZEDBSD_MUSL_ROOT)/src/internal/floatscan.c softfloat/musl-floatscan.h
	@mkdir -p $(dir $@)
	$(SPARCV9_CC) $(ZEDBSD_MUSL_CPPFLAGS) \
		$(SPARCV9_DYNAMIC_CFLAGS) -Wno-error=parentheses \
		-Wno-error=sign-compare -include softfloat/musl-floatscan.h \
		-c $< -o $@

$(SPARCV9_DYNAMIC_FLOAT_DIR)/musl-strtod.o: \
	$(ZEDBSD_MUSL_ROOT)/src/stdlib/strtod.c softfloat/musl-floatscan.h
	@mkdir -p $(dir $@)
	$(SPARCV9_CC) $(ZEDBSD_MUSL_CPPFLAGS) \
		$(SPARCV9_DYNAMIC_CFLAGS) -include softfloat/musl-floatscan.h \
		-c $< -o $@

$(SPARCV9_DYNAMIC_FLOAT_DIR)/musl-compat.o: \
	softfloat/musl-compat.c softfloat/musl-floatscan.h
	@mkdir -p $(dir $@)
	$(SPARCV9_CC) $(ZEDBSD_MUSL_CPPFLAGS) \
		$(SPARCV9_DYNAMIC_CFLAGS) -include softfloat/musl-floatscan.h \
		-c $< -o $@

$(SPARCV9_DYNAMIC_DIR)/ld.so: $(SPARCV9_DYNAMIC_RTLD_OBJS)
	$(SPARCV9_LD) -m elf64_sparc -shared -Bsymbolic -e _rtld_start \
		--hash-style=sysv -z now -z relro -z separate-code \
		-z max-page-size=8192 $^ -o $@

$(SPARCV9_DYNAMIC_DIR)/libc.so: $(SPARCV9_DYNAMIC_LIBC_OBJS)
	$(SPARCV9_CC) $(SPARCV9_DYNAMIC_CFLAGS) -nostdlib -shared \
		-static-libgcc \
		-Wl,-soname,libc.so,--hash-style=sysv,-z,now,-z,relro \
		-Wl,-z,separate-code,-z,max-page-size=8192,-z,stack-size=0x100000 \
		$^ -lgcc -o $@

$(SPARCV9_DYNAMIC_DIR)/tlstest.so: \
	$(SPARCV9_DYNAMIC_DIR)/obj/userland/tests/tlstest.o \
	$(SPARCV9_DYNAMIC_DIR)/ld.so
	$(SPARCV9_LD) -m elf64_sparc -shared -soname tlstest.so \
		--hash-style=sysv -z now -z relro -z separate-code \
		-z max-page-size=8192 $< -o $@

$(SPARCV9_DYNAMIC_DIR)/dyntest: \
	$(SPARCV9_DYNAMIC_DIR)/obj/userland/crt1.o \
	$(SPARCV9_DYNAMIC_DIR)/obj/userland/tests/dyntest.o \
	$(SPARCV9_DYNAMIC_DIR)/libc.so $(SPARCV9_DYNAMIC_DIR)/ld.so \
	$(SPARCV9_DYNAMIC_DIR)/tlstest.so
	$(SPARCV9_CC) $(SPARCV9_DYNAMIC_CFLAGS) -nostdlib -no-pie \
		-Wl,--no-relax,--hash-style=sysv,-z,now,-z,relro \
		-Wl,-z,separate-code,-z,max-page-size=8192,-z,stack-size=0x100000 \
		-Wl,--allow-shlib-undefined,--dynamic-linker=/lib/ld.so \
		$(SPARCV9_DYNAMIC_DIR)/obj/userland/crt1.o \
		$(SPARCV9_DYNAMIC_DIR)/obj/userland/tests/dyntest.o \
		-L$(SPARCV9_DYNAMIC_DIR) -Wl,-rpath-link,$(SPARCV9_DYNAMIC_DIR) \
		-l:libc.so -static-libgcc -lgcc -o $@

dynamic-userland-check: $(SPARCV9_DYNAMIC_DIR)/ld.so \
	$(SPARCV9_DYNAMIC_DIR)/libc.so $(SPARCV9_DYNAMIC_DIR)/dyntest \
	$(SPARCV9_DYNAMIC_DIR)/tlstest.so scripts/check-dynamic-elf.py
	$(PYTHON) scripts/check-dynamic-elf.py --machine sparcv9 \
		--role interpreter $(SPARCV9_DYNAMIC_DIR)/ld.so
	$(PYTHON) scripts/check-dynamic-elf.py --machine sparcv9 \
		--role libc $(SPARCV9_DYNAMIC_DIR)/libc.so
	$(PYTHON) scripts/check-dynamic-elf.py --machine sparcv9 \
		--role module $(SPARCV9_DYNAMIC_DIR)/tlstest.so
	$(PYTHON) scripts/check-dynamic-elf.py --machine sparcv9 \
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
	$(PYTHON) scripts/check-sparcv9-boot.py \
		--stage1 $(BUILD)/boot/stage1.bin \
		--stage2 $(BUILD)/boot/stage2.bin

$(BUILD)/hdd-image.img: $(BUILD)/vmunix $(BUILD)/bin/sh $(BUILD)/bin/sysctl \
	$(SPARCV9_DYNAMIC_DIR)/ld.so $(SPARCV9_DYNAMIC_DIR)/libc.so \
	$(SPARCV9_DYNAMIC_DIR)/tlstest.so $(SPARCV9_DYNAMIC_DIR)/dyntest \
	$(BUILD)/boot/stage1.bin \
	$(BUILD)/boot/stage2.bin scripts/make-sparcv9-hdd-image.py \
	scripts/check-sparcv9-hdd-image.py
	$(PYTHON) scripts/make-sparcv9-hdd-image.py --force \
		--stage1 $(BUILD)/boot/stage1.bin \
		--stage2 $(BUILD)/boot/stage2.bin \
		--kernel $(BUILD)/vmunix --shell $(BUILD)/bin/sh \
		--sysctl $(BUILD)/bin/sysctl \
		--rtld $(SPARCV9_DYNAMIC_DIR)/ld.so \
		--libc $(SPARCV9_DYNAMIC_DIR)/libc.so \
		--tlstest $(SPARCV9_DYNAMIC_DIR)/tlstest.so \
		--dyntest $(SPARCV9_DYNAMIC_DIR)/dyntest $@

$(BUILD)/ufs-root.img: $(BUILD)/bin/sh $(BUILD)/bin/sysctl \
	$(SPARCV9_DYNAMIC_DIR)/ld.so $(SPARCV9_DYNAMIC_DIR)/libc.so \
	$(SPARCV9_DYNAMIC_DIR)/tlstest.so $(SPARCV9_DYNAMIC_DIR)/dyntest \
	scripts/make-ufs1-root-image.py \
	scripts/ufs1_format.py
	$(PYTHON) scripts/make-ufs1-root-image.py --force \
		--arch-profile sparcv9 --native-shell $(BUILD)/bin/sh \
		--native-sysctl $(BUILD)/bin/sysctl \
		--native-rtld $(SPARCV9_DYNAMIC_DIR)/ld.so \
		--native-libc $(SPARCV9_DYNAMIC_DIR)/libc.so \
		--native-tlstest $(SPARCV9_DYNAMIC_DIR)/tlstest.so \
		--native-dyntest $(SPARCV9_DYNAMIC_DIR)/dyntest $@

$(BUILD)/ufs-root-hdd-image.img: $(BUILD)/vmunix $(BUILD)/bin/sh \
	$(BUILD)/bin/sysctl \
	$(BUILD)/boot/stage1.bin $(BUILD)/boot/stage2.bin \
	$(BUILD)/ufs-root.img scripts/make-sparcv9-hdd-image.py \
	scripts/check-sparcv9-hdd-image.py scripts/check-ufs1-image.py
	$(PYTHON) scripts/make-sparcv9-hdd-image.py --force \
		--stage1 $(BUILD)/boot/stage1.bin \
		--stage2 $(BUILD)/boot/stage2.bin \
		--kernel $(BUILD)/vmunix --shell $(BUILD)/bin/sh \
		--sysctl $(BUILD)/bin/sysctl \
		--ufs-root $(BUILD)/ufs-root.img $@

ufs-root-image: $(BUILD)/ufs-root-hdd-image.img

hdd-image: $(BUILD)/hdd-image.img

sparcv9-disk-check: $(BUILD)/hdd-image.img
	$(PYTHON) scripts/check-sparcv9-hdd-image.py \
		--stage1 $(BUILD)/boot/stage1.bin \
		--stage2 $(BUILD)/boot/stage2.bin \
		--kernel $(BUILD)/vmunix --shell $(BUILD)/bin/sh \
		--sysctl $(BUILD)/bin/sysctl \
		--rtld $(SPARCV9_DYNAMIC_DIR)/ld.so \
		--libc $(SPARCV9_DYNAMIC_DIR)/libc.so \
		--tlstest $(SPARCV9_DYNAMIC_DIR)/tlstest.so \
		--dyntest $(SPARCV9_DYNAMIC_DIR)/dyntest $<

sparcv9-entry-qemu-test: $(BUILD)/hdd-image.img
	bash scripts/test-sparcv9-entry-qemu.sh

sparcv9-qemu-test: $(BUILD)/hdd-image.img
	bash scripts/test-sparcv9-qemu.sh

sparcv9-ufs-qemu-test: $(BUILD)/ufs-root-hdd-image.img
	bash scripts/test-sparcv9-ufs-qemu.sh

.PHONY: all vmunix SH POSIX-R1.ELF POSIX-R2.ELF POSIX-R2-REMAINING.ELF hdd-image ufs-root-image sparcv9-toolchain sparcv9-image-check \
	sparcv9-bootloader sparcv9-disk-check sparcv9-entry-qemu-test \
	sparcv9-qemu-test sparcv9-ufs-qemu-test

-include $(SPARCV9_EARLY_OBJS:.o=.d) \
	$(SPARCV9_STAGE1_OBJS:.o=.d) $(SPARCV9_STAGE2_OBJS:.o=.d)
-include $(SPARCV9_KERNEL_OBJS:.o=.d) $(SPARCV9_KERNEL_LIBC_OBJS:.o=.d)
-include $(SPARCV9_USER_OBJS:.o=.d)

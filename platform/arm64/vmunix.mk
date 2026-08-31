# zedBSD arm64/Raspberry Pi 4 bootstrap rules.
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib

ARM64_CC ?= aarch64-linux-gnu-gcc
ARM64_LD ?= aarch64-linux-gnu-ld
ARM64_OBJCOPY ?= aarch64-linux-gnu-objcopy
ARM64_NM ?= aarch64-linux-gnu-nm
ARM64_PLATFORM := platform/arm64

ARM64_CPPFLAGS := -nostdinc -Iinclude -Iinclude/uapi -Isrc -I. \
	-Ilibc/include -Isrc/hal/arm64 -DHAL_ARCH_ARM64 -DHAL_BOARD_RPI4 \
	-DZEDBSD_USER_ABI_AARCH64 -DZEDBSD_USER_ABI_LP64
ARM64_CPPFLAGS += $(ZEDBSD_CONFIG_CPPFLAGS)
ARM64_CFLAGS := -march=armv8-a -mno-outline-atomics -mgeneral-regs-only -ffreestanding \
	-fno-pic -fno-pie -fno-stack-protector -fno-asynchronous-unwind-tables \
	-fno-unwind-tables -fno-common -ffunction-sections -fdata-sections \
	-Os -Wall -Wextra -Werror

ARM64_BOOT_C := src/hal/cpu-up.c src/hal/arm64/asm.c src/hal/arm64/lib.c \
	src/hal/arm64/page.c src/hal/arm64/space.c \
	src/hal/arm64/int.c src/hal/arm64/irq.c \
	src/hal/arm64/task.c \
	src/hal/arm64/cmain.c src/hal/arm64/bsp-rpi4/uart.c \
	src/hal/arm64/bsp-rpi4/cons.c src/hal/arm64/bsp-rpi4/fdt.c \
	src/hal/arm64/bsp-rpi4/mailbox.c src/hal/arm64/bsp-rpi4/framebuffer.c \
	src/hal/arm64/bsp-rpi4/boot.c src/hal/arm64/bsp-rpi4/gic.c \
	src/hal/arm64/bsp-rpi4/clock.c
ARM64_BOOT_S := src/hal/arm64/locore.S src/hal/arm64/trap.S \
	src/hal/arm64/dispatch.S
ARM64_BOOT_OBJS := $(patsubst %.c,$(BUILD)/%.o,$(ARM64_BOOT_C)) \
	$(patsubst %.S,$(BUILD)/%.o,$(ARM64_BOOT_S))

ARM64_KERNEL_SOURCES := \
	src/kern/main.c \
	$(KERN_FAT_SOURCES) \
	src/kern/inode.c src/kern/file.c src/kern/namecache.c src/kern/namei.c \
	src/kern/mount.c src/kern/rootfs.c src/kern/tmpfs.c src/kern/overlayfs.c \
	src/kern/vfs.c src/kern/swap.c src/kern/backing-claim.c src/kern/swap-source.c \
	src/kern/swap-control.c \
	src/kern/swap-boot.c \
	src/kern/swap-fat.c src/kern/vm-reclaim.c src/kern/buf.c \
	src/kern/sysctl.c src/kern/resource.c src/kern/poll.c src/kern/usync.c src/kern/disk.c \
	src/kern/resource-limit.c \
	src/drivers/loop.c \
	src/kern/partition.c src/drivers/disklabel/mbr.c \
	src/kern/platform/rpi4.c \
	src/drivers/rpi4-sdhci.c \
	src/kern/panic.c src/kern/entry.c src/kern/clock.c \
	src/kern/process-timer.c src/kern/klog.c \
	src/kern/lock.c src/kern/waitq.c \
	src/kern/process.c src/kern/thread.c src/kern/sched.c src/kern/vm-lock.c src/kern/vmspace.c \
	src/kern/vm-object.c src/kern/vm-commit.c src/kern/filedesc.c \
	src/kern/record-lock.c \
	src/kern/pipe.c src/kern/cred.c \
	src/kern/signal.c \
	src/kern/cwdinfo.c \
	src/kern/elf.c src/kern/exec.c src/kern/user-probe.c src/kern/syscall.c \
	src/kern/uaccess.c src/kern/cdev.c src/kern/devfs.c \
	src/drivers/fs/console.c src/drivers/input-queue.c \
	src/drivers/input-capability.c src/drivers/input-device.c \
	src/drivers/input-subscriber.c \
	src/drivers/input-keymap.c src/drivers/hid/hid-report.c \
	src/kern/locale-record.c \
	src/kern/tty.c \
	src/kern/system-swap-device.c src/kern/system-device.c src/kern/shutdown.c \
	src/kern/init.c
ARM64_KERNEL_SOURCES += $(KERN_NET_SOURCES) $(KERN_BLOCK_IDENTITY_SOURCES) \
	$(KERN_UFS1_SOURCES) $(KERN_UFS2_SOURCES)
ARM64_KERNEL_SOURCES += $(KERN_BOOT_SOURCES)
ARM64_KERNEL_SOURCES += $(KERN_ACL_SOURCES)
ARM64_KERNEL_SOURCES += $(KERN_QUOTA_SOURCES)
ARM64_KERNEL_OBJS := $(patsubst %.c,$(BUILD)/kernel/%.o,$(ARM64_KERNEL_SOURCES))
ARM64_KERNEL_LIBC_OBJS := $(patsubst %.c,$(BUILD)/kernel/%.o,$(ZEDBSD_LIBC_SOURCES))
ARM64_VMUNIX_OBJS := $(ARM64_BOOT_OBJS) $(ARM64_KERNEL_OBJS) $(ARM64_KERNEL_LIBC_OBJS)

ARM64_USER_CPPFLAGS := -nostdinc -Iinclude -Iinclude/uapi -Isrc -I. \
	-Ilibc/include -DHAL_ARCH_ARM64 -DZEDBSD_USER_ABI_AARCH64 \
	-DZEDBSD_USER_ABI_LP64
ARM64_USER_CFLAGS := -march=armv8-a -mno-outline-atomics \
	-ffreestanding -fno-pic -fno-pie \
	-fno-stack-protector -fno-asynchronous-unwind-tables -fno-unwind-tables \
	-fno-builtin -fno-common -ffunction-sections -fdata-sections \
	-Os -Wall -Wextra -Werror
ARM64_USER_RUNTIME_SOURCES := userland/base/libc/posix.c userland/base/libc/dlfcn.c userland/base/libc/static-tls.c userland/base/libc/poll.c \
	userland/base/libc/termios.c \
	userland/base/libc/pthread.c \
	userland/base/libc/timer.c \
	userland/base/libc/shm.c \
	userland/base/libc/semaphore.c \
	userland/base/libc/mqueue.c \
	userland/base/libc/socket.c \
	userland/base/libc/signal.c userland/base/libc/account.c userland/base/libc/crypt.c \
	userland/base/libc/utmpx.c \
	libc/heap.c libc/string.c libc/ctype.c libc/locale.c libc/wide.c \
	libc/int64.c libc/strto.c \
	libc/format.c libc/stdio.c $(ZEDBSD_LIBC_USER_EXTRA_SOURCES)
ARM64_USER_SH_SOURCES := $(USERLAND_sh_SOURCES)
ARM64_USER_RUNTIME_OBJS := \
	$(patsubst %.c,$(BUILD)/user/%.o,$(ARM64_USER_RUNTIME_SOURCES))
ARM64_USER_SH_OBJS := \
	$(patsubst %.c,$(BUILD)/user/%.o,$(ARM64_USER_SH_SOURCES))
ARM64_USER_READLINE_OBJ := $(BUILD)/user/userland/base/libedit/readline.o
ARM64_USER_READLINE_LIB := $(BUILD)/lib/libreadline.a
ARM64_USER_OBJS := $(BUILD)/user/src/crt/crt0-aarch64.o \
	$(ARM64_USER_RUNTIME_OBJS) $(ARM64_USER_SH_OBJS)

vmunix: $(BUILD)/vmunix
$(BUILD)/tests/rpi4-fdt-host-test: tests/rpi4-fdt-host-test.c \
	src/hal/arm64/bsp-rpi4/fdt.c
	@mkdir -p $(dir $@)
	$(HOSTCC) -std=c11 -O2 -Wall -Wextra -Werror -Iinclude -Isrc \
		src/hal/arm64/bsp-rpi4/fdt.c $< -o $@

rpi4-fdt-host-test: $(BUILD)/tests/rpi4-fdt-host-test
	$< vendor/raspberrypi-firmware/boot/bcm2711-rpi-4-b.dtb

CHECK_RUN_TARGETS += rpi4-fdt-host-test

$(BUILD)/src/hal/cpu-up.o: src/hal/cpu-up.c
	@mkdir -p $(dir $@)
	$(ARM64_CC) $(ARM64_CPPFLAGS) $(ARM64_CFLAGS) -MMD -MP -c $< -o $@

$(BUILD)/src/hal/arm64/%.o: src/hal/arm64/%.c
	@mkdir -p $(dir $@)
	$(ARM64_CC) $(ARM64_CPPFLAGS) $(ARM64_CFLAGS) -MMD -MP -c $< -o $@

$(BUILD)/src/hal/arm64/%.o: src/hal/arm64/%.S
	@mkdir -p $(dir $@)
	$(ARM64_CC) $(ARM64_CPPFLAGS) $(ARM64_CFLAGS) -D_ASM_SRC_ -c $< -o $@

$(BUILD)/kernel/%.o: %.c
	@mkdir -p $(dir $@)
	$(ARM64_CC) $(ARM64_CPPFLAGS) $(ARM64_CFLAGS) -fno-builtin \
		-fno-strict-aliasing -MMD -MP -c $< -o $@

$(BUILD)/kernel/libc/%.o: libc/%.c
	@mkdir -p $(dir $@)
	$(ARM64_CC) $(ARM64_CPPFLAGS) $(filter-out -mgeneral-regs-only,$(ARM64_CFLAGS)) \
		-fno-builtin -fno-strict-aliasing -MMD -MP -c $< -o $@

$(BUILD)/user/%.o: %.c
	@mkdir -p $(dir $@)
	$(ARM64_CC) $(ARM64_USER_CPPFLAGS) $(ARM64_USER_CFLAGS) \
		-fno-strict-aliasing -MMD -MP -c $< -o $@

$(BUILD)/user/src/crt/crt0-aarch64.o: src/crt/crt0-aarch64.S \
	include/hal/arch.h include/hal/arch/aarch64.h
	@mkdir -p $(dir $@)
	$(ARM64_CC) $(ARM64_USER_CPPFLAGS) $(ARM64_USER_CFLAGS) -c $< -o $@

$(ARM64_USER_SH_OBJS) $(ARM64_USER_READLINE_OBJ): \
	ARM64_USER_CPPFLAGS += -Iuserland/base/libedit
$(ARM64_USER_READLINE_LIB): $(ARM64_USER_READLINE_OBJ)
	@mkdir -p $(dir $@)
	$(AR) rcs $@ $^

ARM64_USER_CURSES_OBJS := $(call ZEDBSD_USERLAND_OBJECTS,\
	$(BUILD)/user,curses)
$(BUILD)/lib/libcurses.a: $(ARM64_USER_CURSES_OBJS)
	@mkdir -p $(dir $@)
	$(AR) rcs $@ $^

$(BUILD)/bin/sh: $(ARM64_USER_OBJS) $(ARM64_USER_READLINE_LIB) \
	$(ARM64_PLATFORM)/user.ld \
	tools/build/check-user-elf.py
	@mkdir -p $(dir $@)
	$(ARM64_LD) --gc-sections -nostdlib -static -z max-page-size=4096 \
		-z stack-size=0x100000 -T $(ARM64_PLATFORM)/user.ld \
		$(ARM64_USER_OBJS) $(ARM64_USER_READLINE_LIB) -o $@
	@test -z "$$($(ARM64_NM) -u $@)" || { $(ARM64_NM) -u $@; exit 1; }
	$(PYTHON) tools/build/check-user-elf.py --machine aarch64 $@

ARM64_USER_SYSCTL_OBJ := $(BUILD)/user/userland/base/sysctl/main.o
$(BUILD)/bin/sysctl: $(BUILD)/user/src/crt/crt0-aarch64.o \
	$(ARM64_USER_RUNTIME_OBJS) $(ARM64_USER_SYSCTL_OBJ) \
	$(ARM64_PLATFORM)/user.ld tools/build/check-user-elf.py
	@mkdir -p $(dir $@)
	$(ARM64_LD) --gc-sections -nostdlib -static -z max-page-size=4096 \
		-z stack-size=0x100000 -T $(ARM64_PLATFORM)/user.ld \
		$(BUILD)/user/src/crt/crt0-aarch64.o \
		$(ARM64_USER_RUNTIME_OBJS) $(ARM64_USER_SYSCTL_OBJ) -o $@
	@test -z "$$($(ARM64_NM) -u $@)" || { $(ARM64_NM) -u $@; exit 1; }
	$(PYTHON) tools/build/check-user-elf.py --machine aarch64 $@

ARM64_USER_MOUNT_OBJ := $(BUILD)/user/userland/base/mount/main.o
$(BUILD)/bin/mount: $(BUILD)/user/src/crt/crt0-aarch64.o \
	$(ARM64_USER_RUNTIME_OBJS) $(ARM64_USER_MOUNT_OBJ) \
	$(ARM64_PLATFORM)/user.ld tools/build/check-user-elf.py
	@mkdir -p $(dir $@)
	$(ARM64_LD) --gc-sections -nostdlib -static -z max-page-size=4096 \
		-z stack-size=0x100000 -T $(ARM64_PLATFORM)/user.ld \
		$(BUILD)/user/src/crt/crt0-aarch64.o \
		$(ARM64_USER_RUNTIME_OBJS) $(ARM64_USER_MOUNT_OBJ) -o $@
	@test -z "$$($(ARM64_NM) -u $@)" || { $(ARM64_NM) -u $@; exit 1; }
	$(PYTHON) tools/build/check-user-elf.py --machine aarch64 $@
$(BUILD)/bin/umount: $(BUILD)/bin/mount
	@mkdir -p $(dir $@)
	cp -f $< $@

USER_BASIC_COMMANDS := $(filter $(ZEDBSD_USER_PROGRAMS),$(USERLAND_BASIC_PROGRAMS))
USER_BASIC_TARGETS := $(addprefix $(BUILD)/bin/,$(USER_BASIC_COMMANDS))
ARM64_USER_BASIC_COMMON_OBJ := $(BUILD)/user/userland/base/common/command.o $(BUILD)/user/userland/base/common/pager.o

define ARM64_USER_BASIC_COMMAND
$(BUILD)/bin/$(1): $(BUILD)/user/src/crt/crt0-aarch64.o \
	$(ARM64_USER_RUNTIME_OBJS) $(ARM64_USER_BASIC_COMMON_OBJ) \
	$(call ZEDBSD_USERLAND_OBJECTS,$(BUILD)/user,$(1)) $(ARM64_PLATFORM)/user.ld \
	tools/build/check-user-elf.py
	@mkdir -p $$(dir $$@)
	$(ARM64_LD) --gc-sections -nostdlib -static -z max-page-size=4096 \
		-z stack-size=0x100000 -T $(ARM64_PLATFORM)/user.ld \
		$(BUILD)/user/src/crt/crt0-aarch64.o $(ARM64_USER_RUNTIME_OBJS) \
		$(ARM64_USER_BASIC_COMMON_OBJ) \
		$(call ZEDBSD_USERLAND_OBJECTS,$(BUILD)/user,$(1)) -o $$@
	@test -z "$$$$($(ARM64_NM) -u $$@)" || { $(ARM64_NM) -u $$@; exit 1; }
	$(PYTHON) tools/build/check-user-elf.py --machine aarch64 $$@
endef
$(foreach command,$(USER_BASIC_COMMANDS),\
	$(eval $(call ARM64_USER_BASIC_COMMAND,$(command))))
$(BUILD)/POSIX-R1.ELF: $(BUILD)/user/src/crt/crt0-aarch64.o \
	$(ARM64_USER_RUNTIME_OBJS) \
	$(BUILD)/user/userland/base/tests/syscall-smoke.o \
	$(ARM64_PLATFORM)/user.ld tools/build/check-user-elf.py
	$(ARM64_LD) --gc-sections -nostdlib -static -z max-page-size=4096 \
		-z stack-size=0x100000 -T $(ARM64_PLATFORM)/user.ld \
		$(BUILD)/user/src/crt/crt0-aarch64.o \
		$(ARM64_USER_RUNTIME_OBJS) \
		$(BUILD)/user/userland/base/tests/syscall-smoke.o -o $@
	@test -z "$$($(ARM64_NM) -u $@)" || { $(ARM64_NM) -u $@; exit 1; }
	$(PYTHON) tools/build/check-user-elf.py --machine aarch64 $@

$(BUILD)/POSIX-R2.ELF: $(BUILD)/user/src/crt/crt0-aarch64.o \
	$(ARM64_USER_RUNTIME_OBJS) \
	$(BUILD)/user/userland/base/tests/posix-r2.o \
	$(ARM64_PLATFORM)/user.ld tools/build/check-user-elf.py
	$(ARM64_LD) --gc-sections -nostdlib -static -z max-page-size=4096 \
		-z stack-size=0x100000 -T $(ARM64_PLATFORM)/user.ld \
		$(BUILD)/user/src/crt/crt0-aarch64.o \
		$(ARM64_USER_RUNTIME_OBJS) \
		$(BUILD)/user/userland/base/tests/posix-r2.o -o $@
	@test -z "$$($(ARM64_NM) -u $@)" || { $(ARM64_NM) -u $@; exit 1; }
	$(PYTHON) tools/build/check-user-elf.py --machine aarch64 $@

$(BUILD)/POSIX-R2-REMAINING.ELF: \
	$(BUILD)/user/src/crt/crt0-aarch64.o $(ARM64_USER_RUNTIME_OBJS) \
	$(BUILD)/user/userland/base/tests/posix-r2-remaining.o \
	$(ARM64_PLATFORM)/user.ld tools/build/check-user-elf.py
	$(ARM64_LD) --gc-sections -nostdlib -static -z max-page-size=4096 \
		-z stack-size=0x100000 -T $(ARM64_PLATFORM)/user.ld \
		$(BUILD)/user/src/crt/crt0-aarch64.o \
		$(ARM64_USER_RUNTIME_OBJS) \
		$(BUILD)/user/userland/base/tests/posix-r2-remaining.o -o $@
	@test -z "$$($(ARM64_NM) -u $@)" || { $(ARM64_NM) -u $@; exit 1; }
	$(PYTHON) tools/build/check-user-elf.py --machine aarch64 $@

# ELF64 runtime linker and shared libc for the aarch64 architecture overlay.
DYNAMIC_DIR := $(BUILD)/dynamic
DYNAMIC_CPPFLAGS := -nostdinc -I. -Iinclude -Iinclude/uapi -Ilibc/include \
	-DHAL_ARCH_ARM64 -DZEDBSD_USER_ABI_AARCH64 -DZEDBSD_USER_ABI_LP64 \
	-DZEDBSD_DYNAMIC_LIBC
DYNAMIC_CFLAGS := -march=armv8-a -mno-outline-atomics -Os -ffreestanding \
	-fPIC -fno-builtin -fno-stack-protector \
	-fno-asynchronous-unwind-tables -fno-unwind-tables \
	-ftls-model=global-dynamic -mtls-dialect=trad -Wall -Wextra -Werror
DYNAMIC_LIBC_SOURCES := userland/base/libc/posix.c userland/base/libc/poll.c \
	userland/base/libc/termios.c userland/base/libc/pthread.c userland/base/libc/timer.c userland/base/libc/shm.c \
	userland/base/libc/semaphore.c userland/base/libc/mqueue.c userland/base/libc/dlfcn.c \
	userland/base/libc/socket.c userland/base/libc/signal.c libc/heap.c libc/string.c \
	libc/ctype.c libc/locale.c libc/wide.c libc/int64.c libc/strto.c \
	libc/format.c libc/stdio.c $(ZEDBSD_LIBC_USER_EXTRA_SOURCES)
DYNAMIC_LIBC_OBJS := $(patsubst %.c,$(DYNAMIC_DIR)/obj/%.o,\
	$(DYNAMIC_LIBC_SOURCES)) $(DYNAMIC_DIR)/obj/userland/base/libc/syscall.o
DYNAMIC_RTLD_OBJS := $(DYNAMIC_DIR)/obj/userland/base/rtld/entry.o \
	$(DYNAMIC_DIR)/obj/userland/base/rtld/tlsdesc.o \
	$(DYNAMIC_DIR)/obj/userland/base/rtld/rtld.o \
	$(DYNAMIC_DIR)/obj/userland/base/rtld/string.o
DYNAMIC_FLOAT_DIR := $(DYNAMIC_DIR)/float
DYNAMIC_LIBM_OBJ := $(DYNAMIC_FLOAT_DIR)/math.o
DYNAMIC_FLOAT_PARSE_OBJS := $(DYNAMIC_FLOAT_DIR)/zed-softfloat.o \
	$(DYNAMIC_FLOAT_DIR)/compiler-runtime.o \
	$(DYNAMIC_FLOAT_DIR)/zed-softfloat128.o \
	$(DYNAMIC_FLOAT_DIR)/compiler-runtime128.o \
	$(DYNAMIC_FLOAT_DIR)/float-parse.o
DYNAMIC_LIBC_OBJS += $(DYNAMIC_LIBM_OBJ) $(DYNAMIC_FLOAT_PARSE_OBJS)

$(DYNAMIC_DIR)/obj/%.o: %.c
	@mkdir -p $(dir $@)
	$(ARM64_CC) $(DYNAMIC_CPPFLAGS) $(DYNAMIC_CFLAGS) -MMD -MP -c $< -o $@
$(DYNAMIC_DIR)/obj/userland/base/libc/syscall.o: \
	userland/base/libc/syscall-aarch64.S include/hal/arch.h \
	include/hal/arch/aarch64.h
	@mkdir -p $(dir $@)
	$(ARM64_CC) $(DYNAMIC_CPPFLAGS) -c $< -o $@
$(DYNAMIC_DIR)/obj/userland/base/rtld/entry.o: userland/base/rtld/entry-aarch64.S
	@mkdir -p $(dir $@)
	$(ARM64_CC) -c $< -o $@
$(DYNAMIC_DIR)/obj/userland/base/rtld/tlsdesc.o: userland/base/rtld/tlsdesc-arm64.S
	@mkdir -p $(dir $@)
	$(ARM64_CC) -c $< -o $@
$(DYNAMIC_DIR)/obj/userland/base/tests/tlstest.o: DYNAMIC_CFLAGS += -mtls-dialect=desc
$(DYNAMIC_DIR)/obj/src/crt/crt1.o: src/crt/crt1-aarch64.S
	@mkdir -p $(dir $@)
	$(ARM64_CC) -c $< -o $@
$(DYNAMIC_LIBM_OBJ): libc/math.c src/softfloat/zed-softfloat.h
	@mkdir -p $(dir $@)
	$(ARM64_CC) -nostdinc -Ilibc/include -Iinclude/uapi -I. \
		$(DYNAMIC_CFLAGS) -c $< -o $@
$(DYNAMIC_FLOAT_DIR)/zed-softfloat.o: src/softfloat/zed-softfloat.c src/softfloat/zed-softfloat.h
	@mkdir -p $(dir $@)
	$(ARM64_CC) -nostdinc -Ilibc/include -Iinclude/uapi -I. \
		$(DYNAMIC_CFLAGS) -c $< -o $@
$(DYNAMIC_FLOAT_DIR)/compiler-runtime.o: src/softfloat/compiler-runtime.c src/softfloat/zed-softfloat.h
	@mkdir -p $(dir $@)
	$(ARM64_CC) -nostdinc -Ilibc/include -Iinclude/uapi -I. \
		$(DYNAMIC_CFLAGS) -c $< -o $@
$(DYNAMIC_FLOAT_DIR)/zed-softfloat128.o: src/softfloat/zed-softfloat128.c \
	src/softfloat/zed-softfloat128.h src/softfloat/zed-softfloat.h
	@mkdir -p $(dir $@)
	$(ARM64_CC) -nostdinc -Ilibc/include -Iinclude/uapi -I. \
		$(DYNAMIC_CFLAGS) -c $< -o $@
$(DYNAMIC_FLOAT_DIR)/compiler-runtime128.o: src/softfloat/compiler-runtime128.c \
	src/softfloat/zed-softfloat128.h src/softfloat/zed-softfloat.h
	@mkdir -p $(dir $@)
	$(ARM64_CC) -nostdinc -Ilibc/include -Iinclude/uapi -I. \
		$(DYNAMIC_CFLAGS) -c $< -o $@
$(DYNAMIC_FLOAT_DIR)/float-parse.o: libc/float-parse.c src/softfloat/zed-softfloat.h
	@mkdir -p $(dir $@)
	$(ARM64_CC) -nostdinc -Ilibc/include -Iinclude/uapi -I. \
		$(DYNAMIC_CFLAGS) -c $< -o $@

$(DYNAMIC_DIR)/ld.so: $(DYNAMIC_RTLD_OBJS)
	$(ARM64_LD) -shared -Bsymbolic -e _rtld_start --hash-style=sysv \
		-z now -z relro -z separate-code -z max-page-size=4096 $^ -o $@
$(DYNAMIC_DIR)/libc.so: $(DYNAMIC_LIBC_OBJS)
	$(ARM64_CC) $(DYNAMIC_CFLAGS) -nostdlib -shared \
		-Wl,-soname,libc.so,--hash-style=both,-z,now,-z,relro \
		-Wl,-z,separate-code,-z,max-page-size=4096,-z,stack-size=0x100000 \
		$^ -o $@
$(DYNAMIC_DIR)/alt/rpathdep.so: \
	$(DYNAMIC_DIR)/obj/userland/base/tests/rpathdep.o $(DYNAMIC_DIR)/ld.so
	@mkdir -p $(dir $@)
	$(ARM64_LD) -shared -soname rpathdep.so --hash-style=gnu \
		-z now -z relro -z separate-code -z max-page-size=4096 $< -o $@
$(DYNAMIC_DIR)/tlstest.so: $(DYNAMIC_DIR)/obj/userland/base/tests/tlstest.o \
	$(DYNAMIC_DIR)/alt/rpathdep.so $(DYNAMIC_DIR)/ld.so
	$(ARM64_LD) -shared -soname tlstest.so --hash-style=gnu \
		-z now -z relro -z separate-code -z max-page-size=4096 \
		--enable-new-dtags -rpath '$$ORIGIN/alt' \
		$(DYNAMIC_DIR)/obj/userland/base/tests/tlstest.o \
		-L$(DYNAMIC_DIR)/alt -l:rpathdep.so -o $@
$(DYNAMIC_DIR)/rpathtest.so: \
	$(DYNAMIC_DIR)/obj/userland/base/tests/rpathtest.o \
	$(DYNAMIC_DIR)/alt/rpathdep.so $(DYNAMIC_DIR)/ld.so
	$(ARM64_LD) -shared -soname rpthtest.so --hash-style=gnu \
		-z now -z relro -z separate-code -z max-page-size=4096 \
		--disable-new-dtags -rpath '$$ORIGIN/alt' $< \
		-L$(DYNAMIC_DIR)/alt -l:rpathdep.so -o $@
$(DYNAMIC_DIR)/verstest.so: \
	$(DYNAMIC_DIR)/obj/userland/base/tests/versiontest.o \
	userland/base/tests/versiontest.map $(DYNAMIC_DIR)/ld.so
	$(ARM64_LD) -shared -soname verstest.so --hash-style=gnu \
		-z now -z relro -z separate-code -z max-page-size=4096 \
		--version-script=userland/base/tests/versiontest.map $< -o $@
$(DYNAMIC_DIR)/versuse.so: \
	$(DYNAMIC_DIR)/obj/userland/base/tests/versionuse.o \
	$(DYNAMIC_DIR)/verstest.so $(DYNAMIC_DIR)/ld.so
	$(ARM64_LD) -shared -soname versuse.so --hash-style=gnu \
		-z now -z relro -z separate-code -z max-page-size=4096 \
		$< -L$(DYNAMIC_DIR) -l:verstest.so -o $@
$(DYNAMIC_DIR)/dyntest: $(DYNAMIC_DIR)/obj/src/crt/crt1.o \
	$(DYNAMIC_DIR)/obj/userland/base/tests/dyntest.o $(DYNAMIC_DIR)/libc.so \
	$(DYNAMIC_DIR)/ld.so $(DYNAMIC_DIR)/tlstest.so \
	$(DYNAMIC_DIR)/versuse.so
	$(ARM64_CC) -nostdlib -pie -Wl,--no-relax,--hash-style=sysv,-z,now,-z,relro \
		-Wl,-z,separate-code,-z,stack-size=0x100000,--allow-shlib-undefined \
		-Wl,--dynamic-linker=/lib/ld.so $(DYNAMIC_DIR)/obj/src/crt/crt1.o \
		$(DYNAMIC_DIR)/obj/userland/base/tests/dyntest.o -L$(DYNAMIC_DIR) \
		-Wl,-rpath-link,$(DYNAMIC_DIR) -l:libc.so -o $@
dynamic-userland-check: $(DYNAMIC_DIR)/ld.so $(DYNAMIC_DIR)/libc.so \
	$(DYNAMIC_DIR)/dyntest $(DYNAMIC_DIR)/tlstest.so \
	$(DYNAMIC_DIR)/rpathtest.so $(DYNAMIC_DIR)/verstest.so \
	$(DYNAMIC_DIR)/versuse.so tools/build/check-dynamic-elf.py
	$(PYTHON) tools/build/check-dynamic-elf.py --machine aarch64 --role interpreter $(DYNAMIC_DIR)/ld.so
	$(PYTHON) tools/build/check-dynamic-elf.py --machine aarch64 --role libc $(DYNAMIC_DIR)/libc.so
	$(PYTHON) tools/build/check-dynamic-elf.py --machine aarch64 --role module $(DYNAMIC_DIR)/tlstest.so
	$(PYTHON) tools/build/check-dynamic-elf.py --machine aarch64 --role rpath-module $(DYNAMIC_DIR)/rpathtest.so
	$(PYTHON) tools/build/check-dynamic-elf.py --machine aarch64 --role version-definition $(DYNAMIC_DIR)/verstest.so
	$(PYTHON) tools/build/check-dynamic-elf.py --machine aarch64 --role version-consumer $(DYNAMIC_DIR)/versuse.so
	$(PYTHON) tools/build/check-dynamic-elf.py --machine aarch64 --role program $(DYNAMIC_DIR)/dyntest
	@echo "zedBSD aarch64 dynamic userland artifacts: PASS"
.PHONY: dynamic-userland-check

AARCH64_ARCH_IMAGE := $(ARCH_IMAGE_DIR)/aarch64.img
AARCH64_ARCH_INPUTS := $(BUILD)/bin/sh $(BUILD)/bin/sysctl \
	$(BUILD)/bin/mount $(BUILD)/bin/umount \
	$(DYNAMIC_DIR)/ld.so $(DYNAMIC_DIR)/libc.so \
	$(DYNAMIC_DIR)/tlstest.so $(DYNAMIC_DIR)/dyntest \
	$(DYNAMIC_DIR)/alt/rpathdep.so $(DYNAMIC_DIR)/rpathtest.so \
	$(DYNAMIC_DIR)/verstest.so $(DYNAMIC_DIR)/versuse.so
AARCH64_ARCH_FILES := --file /bin/sh=$(BUILD)/bin/sh \
	--file /sbin/sysctl=$(BUILD)/bin/sysctl \
	--file /sbin/mount=$(BUILD)/bin/mount \
	--file /sbin/umount=$(BUILD)/bin/umount \
	--file /lib/ld.so=$(DYNAMIC_DIR)/ld.so \
	--file /lib/libc.so=$(DYNAMIC_DIR)/libc.so \
	--file /lib/tlstest.so=$(DYNAMIC_DIR)/tlstest.so \
	--file /lib/alt/rpathdep.so=$(DYNAMIC_DIR)/alt/rpathdep.so \
	--file /lib/rpthtest.so=$(DYNAMIC_DIR)/rpathtest.so \
	--file /lib/verstest.so=$(DYNAMIC_DIR)/verstest.so \
	--file /lib/versuse.so=$(DYNAMIC_DIR)/versuse.so \
	--file /bin/dyntest=$(DYNAMIC_DIR)/dyntest
AARCH64_ARCH_INPUTS += $(USER_BASIC_TARGETS)
AARCH64_ARCH_FILES += $(foreach command,$(USER_BASIC_COMMANDS),--file $(call zedbsd_userland_destination,$(command))=$(BUILD)/bin/$(command))
AARCH64_ARCH_FILES += $(ZEDBSD_USERLAND_FILE_MODES)
AARCH64_ARCH_INPUTS += $(ZEDBSD_ACCOUNT_INPUTS)
AARCH64_ARCH_FILES += $(ZEDBSD_ACCOUNT_FILES)
AARCH64_ARCH_INPUTS += $(ZEDBSD_BASE_DATA_INPUTS)
AARCH64_ARCH_FILES += $(ZEDBSD_BASE_DATA_FILES)
$(eval $(call ZEDBSD_ARCH_IMAGE_RULE,$(AARCH64_ARCH_IMAGE),aarch64,$(AARCH64_ARCH_INPUTS),$(AARCH64_ARCH_FILES)))
$(eval $(call ZEDBSD_ROOTFS_TAR_RULE,$(BUILD)/rootfs.tar.gz,$(AARCH64_ARCH_INPUTS),$(AARCH64_ARCH_FILES)))
AARCH64_ARCH_UFS_IMAGE := $(ARCH_IMAGE_DIR)/aarch64.ufs
$(eval $(call ZEDBSD_ARCH_UFS_IMAGE_RULE,$(AARCH64_ARCH_UFS_IMAGE),aarch64,$(AARCH64_ARCH_INPUTS),$(AARCH64_ARCH_FILES)))
rootfs: $(BUILD)/rootfs/.stamp

$(BUILD)/ufs-root.img: $(AARCH64_ARCH_UFS_IMAGE) \
	tools/build/make-ufs1-root-image.py tools/build/ufs1_format.py
	$(PYTHON) tools/build/make-ufs1-root-image.py --force \
		--arch-profile aarch64 --arch-image $(AARCH64_ARCH_UFS_IMAGE) $@

$(BUILD)/ufs-root-hdd-image.img: $(BUILD)/vmunix \
	$(AARCH64_ARCH_IMAGE) $(BUILD)/ufs-root.img \
	$(ARM64_PLATFORM)/config.txt \
	platform/arm64/tools/make-rpi4-ufs-root-hdd-image.py \
	platform/arm64/tools/make-rpi4-hdd-image.py tools/build/check-ufs1-image.py
	$(PYTHON) platform/arm64/tools/make-rpi4-ufs-root-hdd-image.py --force \
		--kernel $(BUILD)/vmunix --arch-image $(AARCH64_ARCH_IMAGE) \
		--ufs-root $(BUILD)/ufs-root.img \
		--config $(ARM64_PLATFORM)/config.txt \
		--firmware-dir vendor/raspberrypi-firmware/boot $@

$(BUILD)/hdd-image.img: $(BUILD)/vmunix $(AARCH64_ARCH_IMAGE) $(DATA_IMAGE) $(SWAP_IMAGE) \
	$(ARM64_PLATFORM)/config.txt platform/arm64/tools/make-rpi4-hdd-image.py \
	platform/arm64/tools/check-rpi4-hdd-image.py
	$(PYTHON) platform/arm64/tools/make-rpi4-hdd-image.py --force \
		--kernel $(BUILD)/vmunix --arch-image $(AARCH64_ARCH_IMAGE) \
		--data-image $(DATA_IMAGE) \
		--swapfile $(SWAP_IMAGE) \
		--config $(ARM64_PLATFORM)/config.txt \
		--firmware-dir vendor/raspberrypi-firmware/boot $@

$(BUILD)/kernel.elf: $(ARM64_VMUNIX_OBJS) $(ARM64_PLATFORM)/vmunix.ld \
	platform/arm64/tools/check-arm64-vmunix.py
	$(ARM64_LD) --gc-sections -z max-page-size=4096 \
		-T $(ARM64_PLATFORM)/vmunix.ld -nostdlib $(ARM64_VMUNIX_OBJS) -o $@
	@test -z "$$($(ARM64_NM) -u $@)" || { $(ARM64_NM) -u $@; exit 1; }
	$(PYTHON) platform/arm64/tools/check-arm64-vmunix.py --elf $@

$(BUILD)/vmunix: $(BUILD)/kernel.elf \
	platform/arm64/tools/check-arm64-vmunix.py
	$(ARM64_OBJCOPY) -O binary $< $@
	$(PYTHON) platform/arm64/tools/check-arm64-vmunix.py \
		--elf $< --image $@ --fix-image
	$(PYTHON) platform/arm64/tools/check-arm64-vmunix.py --elf $< --image $@

.PHONY: rpi4-fdt-host-test

-include $(ARM64_BOOT_OBJS:.o=.d) $(ARM64_KERNEL_OBJS:.o=.d) \
	$(ARM64_KERNEL_LIBC_OBJS:.o=.d)

-include $(ARM64_USER_OBJS:.o=.d)

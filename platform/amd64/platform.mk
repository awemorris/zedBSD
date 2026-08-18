# zedBSD amd64/PC-AT bootstrap rules.
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib

AMD64_PLATFORM := platform/amd64
BIOS_LOADER := bootloader/pcat

AMD64_CPPFLAGS := -nostdinc -Iinclude -Iinclude/uapi -Isrc -I. \
	-Ilibc/include -DHAL_ARCH_AMD64 -DHAL_BOARD_PCAT -DHAL_PCAT_DEBUGCON \
	-DZEDBSD_USER_ABI_LP64 \
	-DPCAT_VGA_APERTURE_ADDRESS=0xffffffff800a0000ULL \
	-DPCAT_CIRRUS_APERTURE_ADDRESS=0xffffffffc0000000ULL
AMD64_CPPFLAGS += $(ZEDBSD_CONFIG_CPPFLAGS)
AMD64_CFLAGS := -m64 -mcmodel=kernel -mno-red-zone -mgeneral-regs-only \
	-ffreestanding -fno-pic -fno-pie -fno-stack-protector \
	-fno-asynchronous-unwind-tables -fno-unwind-tables \
	-Os -Wall -Wextra -Werror
AMD64_KERNEL_LIBC_CFLAGS := $(filter-out -mgeneral-regs-only,$(AMD64_CFLAGS))

AMD64_HAL_SOURCES := src/hal/x86/rtc.c src/hal/amd64/asm.c src/hal/amd64/lib.c \
	src/hal/amd64/page.c src/hal/amd64/space.c src/hal/amd64/cmain.c \
	src/hal/amd64/descriptor.c src/hal/amd64/int.c src/hal/amd64/irq.c \
	src/hal/amd64/task.c src/hal/amd64/percpu.c src/hal/amd64/smp.c \
	src/hal/amd64/bsp-pcat/boot.c src/hal/amd64/bsp-pcat/cons.c \
	src/hal/amd64/bsp-pcat/pic.c src/hal/amd64/bsp-pcat/clock.c \
	src/hal/amd64/bsp-pcat/acpi.c src/hal/amd64/bsp-pcat/lapic.c \
	src/hal/amd64/bsp-pcat/ioapic.c
AMD64_HAL_ASM := src/hal/amd64/locore.S src/hal/amd64/trap.S \
	src/hal/amd64/dispatch.S src/hal/amd64/ap-trampoline.S
AMD64_HAL_OBJS := $(patsubst %.c,$(BUILD)/%.o,$(AMD64_HAL_SOURCES)) \
	$(patsubst %.S,$(BUILD)/%.o,$(AMD64_HAL_ASM))

AMD64_KERNEL_SOURCES := \
	src/kern/main.c src/kern/env.c src/kern/fs.c src/kern/namespace.c \
	src/kern/fat.c src/kern/fat-lfn.c src/kern/fat16.c \
	src/kern/fat-vfs.c src/kern/inode.c src/kern/file.c \
	src/kern/namecache.c src/kern/namei.c src/kern/mount.c \
	src/kern/rootfs.c src/kern/tmpfs.c src/kern/overlayfs.c src/kern/vfs.c \
	src/kern/swap.c src/kern/swap-fat.c \
	src/kern/vm-reclaim.c src/kern/buf.c src/kern/sysctl.c \
	src/kern/resource.c src/kern/poll.c src/kern/usync.c \
	src/kern/resource-limit.c \
	src/kern/disk.c src/kern/partition.c \
	drivers/loop.c \
	drivers/pcat-ide.c drivers/dp8390.c drivers/pcat-ne2000.c \
	src/kern/mbr-partition.c src/kern/pcat/platform.c \
	src/kern/image.c src/kern/panic.c src/kern/entry.c src/kern/clock.c \
	src/kern/process-timer.c src/kern/klog.c \
	src/kern/test-checkpoint.c \
	src/kern/lock.c src/kern/waitq.c \
	src/kern/process.c src/kern/thread.c src/kern/sched.c \
	src/kern/vmspace.c src/kern/vm-object.c src/kern/vm-commit.c \
	src/kern/filedesc.c \
	src/kern/record-lock.c \
	src/kern/pipe.c src/kern/cred.c src/kern/signal.c \
	src/kern/cwdinfo.c src/kern/elf.c src/kern/exec.c \
	src/kern/user-probe.c src/kern/syscall.c src/kern/uaccess.c \
	src/kern/cdev.c src/kern/devfs.c src/kern/console-device.c src/kern/tty.c \
	src/kern/graphics-device.c src/kern/system-device.c \
	src/kern/pcat/font.c drivers/pcat-graphics.c \
	src/kern/init.c
AMD64_KERNEL_SOURCES += $(KERN_NET_SOURCES) $(KERN_UFS1_SOURCES) \
	$(KERN_UFS2_SOURCES) $(KERN_UFS_CONSISTENCY_SOURCES)
AMD64_KERNEL_SOURCES += $(KERN_ACL_SOURCES)
AMD64_KERNEL_SOURCES += $(KERN_QUOTA_SOURCES)
AMD64_KERNEL_OBJS := $(patsubst %.c,$(BUILD)/kern64/%.o,\
	$(AMD64_KERNEL_SOURCES))
AMD64_KERNEL_LIBC_OBJS := $(patsubst %.c,$(BUILD)/kern64/%.o,\
	$(ZEDBSD_LIBC_SOURCES))
AMD64_VMUNIX_OBJS := $(AMD64_HAL_OBJS) $(AMD64_KERNEL_OBJS) \
	$(AMD64_KERNEL_LIBC_OBJS)

all: $(BUILD)/vmunix $(BUILD)/bin/sh $(BUILD)/bin/nettest \
	$(BUILD)/bin/ping $(BUILD)/bin/ifconfig $(BUILD)/bin/route \
	$(BUILD)/bin/dhcpcd $(BUILD)/bin/nslookup $(BUILD)/bin/host $(BUILD)/bin/sysctl \
	$(BUILD)/bin/mount $(BUILD)/bin/umount \
	$(BUILD)/hdd-image.img
vmunix: $(BUILD)/vmunix
SH: $(BUILD)/bin/sh
POSIX-R1.ELF: $(BUILD)/POSIX-R1.ELF
POSIX-R2.ELF: $(BUILD)/POSIX-R2.ELF
POSIX-R2-REMAINING.ELF: $(BUILD)/POSIX-R2-REMAINING.ELF
SMP-STRESS.ELF: $(BUILD)/SMP-STRESS.ELF
.PHONY: POSIX-R1.ELF POSIX-R2.ELF POSIX-R2-REMAINING.ELF SMP-STRESS.ELF

$(BUILD)/src/hal/amd64/%.o: src/hal/amd64/%.S
	@mkdir -p $(dir $@)
	$(CC) $(AMD64_CPPFLAGS) $(AMD64_CFLAGS) -D_ASM_SRC_ -c $< -o $@

$(BUILD)/src/hal/amd64/%.o: src/hal/amd64/%.c
	@mkdir -p $(dir $@)
	$(CC) $(AMD64_CPPFLAGS) $(AMD64_CFLAGS) -MMD -MP -c $< -o $@

# Shared x86 HAL sources must use the amd64 flags as well.  Without this
# rule the generic i386 pattern can leave a 32-bit object in build/amd64.
$(BUILD)/src/hal/x86/%.o: src/hal/x86/%.c
	@mkdir -p $(dir $@)
	$(CC) $(AMD64_CPPFLAGS) $(AMD64_CFLAGS) -MMD -MP -c $< -o $@

$(BUILD)/kern64/src/kern/%.o: src/kern/%.c
	@mkdir -p $(dir $@)
	$(CC) $(AMD64_CPPFLAGS) $(AMD64_CFLAGS) -MMD -MP -c $< -o $@

$(BUILD)/kern64/drivers/%.o: drivers/%.c
	@mkdir -p $(dir $@)
	$(CC) $(AMD64_CPPFLAGS) $(AMD64_CFLAGS) -MMD -MP -c $< -o $@

$(BUILD)/kern64/%.o: %.c
	@mkdir -p $(dir $@)
	$(CC) $(AMD64_CPPFLAGS) $(AMD64_KERNEL_LIBC_CFLAGS) -fno-builtin \
		-fno-strict-aliasing -MMD -MP -c $< -o $@

$(BUILD)/vmunix: $(AMD64_VMUNIX_OBJS) $(AMD64_PLATFORM)/vmunix.ld \
	scripts/check-amd64-vmunix.py
	$(LD) -m elf_x86_64 --gc-sections -z max-page-size=4096 \
		-T $(AMD64_PLATFORM)/vmunix.ld -nostdlib $(AMD64_VMUNIX_OBJS) -o $@
	$(PYTHON) scripts/check-amd64-vmunix.py $@

$(BUILD)/bootloader/stage1.o: $(BIOS_LOADER)/stage1.S \
	bootloader/include/disk-layout.inc bootloader/include/stage2-header.inc
	@mkdir -p $(dir $@)
	$(CC) -m32 -I. -x assembler-with-cpp -c $< -o $@

$(BUILD)/bootloader/stage1.elf: $(BUILD)/bootloader/stage1.o \
	$(BIOS_LOADER)/stage1.ld
	$(LD) -m elf_i386 -T $(BIOS_LOADER)/stage1.ld $< -o $@

$(BUILD)/bootloader/stage1.bin: $(BUILD)/bootloader/stage1.elf
	$(OBJCOPY) -O binary -j .text $< $@
	@test $$(stat -c%s $@) -eq 512

$(BUILD)/bootloader/stage2.o: $(BIOS_LOADER)/stage2.S \
	bootloader/include/disk-layout.inc bootloader/include/stage2-header.inc \
	bootloader/include/mbr.inc bootloader/include/fat16.inc \
	bootloader/include/elf.inc bootloader/include/amd64-handoff.h
	@mkdir -p $(dir $@)
	$(CC) -m64 -I. -x assembler-with-cpp -c $< -o $@

$(BUILD)/bootloader/stage2.elf: $(BUILD)/bootloader/stage2.o \
	$(BIOS_LOADER)/stage2.ld
	$(LD) -m elf_x86_64 -T $(BIOS_LOADER)/stage2.ld $< -o $@

$(BUILD)/bootloader/stage2.raw: $(BUILD)/bootloader/stage2.elf
	$(OBJCOPY) -O binary -j .text $< $@

$(BUILD)/bootloader/stage2.bin: $(BUILD)/bootloader/stage2.raw \
	scripts/finalize-bios-stage2.py
	$(PYTHON) scripts/finalize-bios-stage2.py --machine pcat $< $@

bios-bootloader: $(BUILD)/bootloader/stage1.bin \
	$(BUILD)/bootloader/stage2.bin

AMD64_USER_CPPFLAGS := -nostdinc -Iinclude -Iinclude/uapi -Isrc -I. \
	-Ilibc/include -DHAL_ARCH_AMD64 -DZEDBSD_USER_ABI_LP64
AMD64_USER_CFLAGS := -m64 -march=x86-64 -mno-red-zone -ffreestanding \
	-fno-pic -fno-pie -fno-stack-protector -fno-asynchronous-unwind-tables \
	-fno-unwind-tables -fno-builtin -fno-common -ffunction-sections \
	-fdata-sections -Os -Wall -Wextra -Werror
AMD64_USER_RUNTIME_SOURCES := userland/libc/posix.c userland/libc/dlfcn.c userland/libc/static-tls.c userland/libc/poll.c \
	userland/libc/termios.c \
	userland/libc/pthread.c \
	userland/libc/shm.c \
	userland/libc/semaphore.c \
	userland/libc/mqueue.c \
	userland/libc/signal.c userland/libc/account.c userland/libc/crypt.c \
	userland/libc/utmpx.c libc/heap.c libc/string.c libc/ctype.c \
	libc/locale.c libc/wide.c \
	libc/int64.c libc/strto.c libc/format.c libc/stdio.c
AMD64_USER_LIBC_OBJS := $(BUILD)/user64/userland/crt0-amd64.o \
	$(patsubst %.c,$(BUILD)/user64/%.o,$(AMD64_USER_RUNTIME_SOURCES))
AMD64_USER_NET_LIBC_OBJS := $(AMD64_USER_LIBC_OBJS) \
	$(BUILD)/user64/userland/libc/socket.o \
	$(BUILD)/user64/userland/libc/resolver.o \
	$(BUILD)/user64/userland/libc/resolver-dns.o
AMD64_USER_NETTEST_OBJS := $(BUILD)/user64/userland/nettest/main.o
AMD64_USER_SH_OBJS := $(BUILD)/user64/userland/sh/main.o \
	$(BUILD)/user64/userland/sh/builtins.o \
	$(BUILD)/user64/userland/sh/lexer.o \
	$(BUILD)/user64/userland/sh/expand.o \
	$(BUILD)/user64/userland/sh/glob.o \
	$(BUILD)/user64/userland/sh/vars.o \
	$(BUILD)/user64/userland/sh/arithmetic.o \
	$(BUILD)/user64/userland/sh/alias.o
AMD64_USER_READLINE_OBJ := $(BUILD)/user64/userland/libedit/readline.o
AMD64_USER_READLINE_LIB := $(BUILD)/lib/libreadline.a
AMD64_USER_ELF_CHECK := scripts/check-user-elf.py

$(BUILD)/user64/%.o: %.c
	@mkdir -p $(dir $@)
	$(CC) $(AMD64_USER_CPPFLAGS) $(AMD64_USER_CFLAGS) \
		-fno-strict-aliasing -MMD -MP -c $< -o $@

$(BUILD)/user64/userland/crt0-amd64.o: userland/crt0-amd64.S
	@mkdir -p $(dir $@)
	$(CC) $(AMD64_USER_CPPFLAGS) $(AMD64_USER_CFLAGS) -c $< -o $@

$(AMD64_USER_READLINE_OBJ): AMD64_USER_CPPFLAGS += -Iuserland/libedit
$(AMD64_USER_SH_OBJS): AMD64_USER_CPPFLAGS += -Iuserland/libedit
$(AMD64_USER_READLINE_LIB): $(AMD64_USER_READLINE_OBJ)
	@mkdir -p $(dir $@)
	$(AR) rcs $@ $^

$(BUILD)/POSIX-R1.ELF: $(AMD64_USER_LIBC_OBJS) \
	$(BUILD)/user64/userland/tests/syscall-smoke.o $(AMD64_PLATFORM)/user.ld \
	$(AMD64_USER_ELF_CHECK)
	$(LD) -m elf_x86_64 --gc-sections -nostdlib -static \
		-z max-page-size=4096 -z stack-size=0x100000 \
		-T $(AMD64_PLATFORM)/user.ld \
		$(AMD64_USER_LIBC_OBJS) \
		$(BUILD)/user64/userland/tests/syscall-smoke.o -o $@
	$(PYTHON) $(AMD64_USER_ELF_CHECK) --machine amd64 $@

$(BUILD)/POSIX-R2.ELF: $(AMD64_USER_NET_LIBC_OBJS) \
	$(BUILD)/user64/userland/tests/posix-r2.o $(AMD64_PLATFORM)/user.ld \
	$(AMD64_USER_ELF_CHECK)
	$(LD) -m elf_x86_64 --gc-sections -nostdlib -static \
		-z max-page-size=4096 -z stack-size=0x100000 \
		-T $(AMD64_PLATFORM)/user.ld $(AMD64_USER_NET_LIBC_OBJS) \
		$(BUILD)/user64/userland/tests/posix-r2.o -o $@
	$(PYTHON) $(AMD64_USER_ELF_CHECK) --machine amd64 $@

$(BUILD)/POSIX-R2-REMAINING.ELF: $(AMD64_USER_NET_LIBC_OBJS) \
	$(BUILD)/user64/userland/tests/posix-r2-remaining.o \
	$(AMD64_PLATFORM)/user.ld $(AMD64_USER_ELF_CHECK)
	$(LD) -m elf_x86_64 --gc-sections -nostdlib -static \
		-z max-page-size=4096 -z stack-size=0x100000 \
		-T $(AMD64_PLATFORM)/user.ld $(AMD64_USER_NET_LIBC_OBJS) \
		$(BUILD)/user64/userland/tests/posix-r2-remaining.o -o $@
	$(PYTHON) $(AMD64_USER_ELF_CHECK) --machine amd64 $@

$(BUILD)/bin/sh: $(AMD64_USER_LIBC_OBJS) $(AMD64_USER_SH_OBJS) \
	$(AMD64_USER_READLINE_LIB) \
	$(AMD64_PLATFORM)/user.ld \
	$(AMD64_USER_ELF_CHECK)
	@mkdir -p $(dir $@)
	$(LD) -m elf_x86_64 --gc-sections -nostdlib -static \
		-z max-page-size=4096 -z stack-size=0x100000 \
		-T $(AMD64_PLATFORM)/user.ld $(AMD64_USER_LIBC_OBJS) \
		$(AMD64_USER_SH_OBJS) $(AMD64_USER_READLINE_LIB) -o $@
	@test -z "$$(nm -u $@)" || { nm -u $@; exit 1; }
	$(PYTHON) $(AMD64_USER_ELF_CHECK) --machine amd64 $@

$(BUILD)/SMP-STRESS.ELF: $(AMD64_USER_NET_LIBC_OBJS) \
	$(BUILD)/user64/userland/tests/smp-resource-stress.o \
	$(AMD64_PLATFORM)/user.ld $(AMD64_USER_ELF_CHECK)
	$(LD) -m elf_x86_64 --gc-sections -nostdlib -static \
		-z max-page-size=4096 -z stack-size=0x100000 \
		-T $(AMD64_PLATFORM)/user.ld $(AMD64_USER_NET_LIBC_OBJS) \
		$(BUILD)/user64/userland/tests/smp-resource-stress.o -o $@
	@test -z "$$(nm -u $@)" || { nm -u $@; exit 1; }
	$(PYTHON) $(AMD64_USER_ELF_CHECK) --machine amd64 $@

AMD64_USER_SYSCTL_OBJ := $(BUILD)/user64/userland/sysctl/main.o
$(BUILD)/bin/sysctl: $(AMD64_USER_LIBC_OBJS) $(AMD64_USER_SYSCTL_OBJ) \
	$(AMD64_PLATFORM)/user.ld $(AMD64_USER_ELF_CHECK)
	@mkdir -p $(dir $@)
	$(LD) -m elf_x86_64 --gc-sections -nostdlib -static \
		-z max-page-size=4096 -z stack-size=0x100000 \
		-T $(AMD64_PLATFORM)/user.ld $(AMD64_USER_LIBC_OBJS) \
		$(AMD64_USER_SYSCTL_OBJ) -o $@
	@test -z "$$(nm -u $@)" || { nm -u $@; exit 1; }
	$(PYTHON) $(AMD64_USER_ELF_CHECK) --machine amd64 $@

AMD64_USER_MOUNT_OBJ := $(BUILD)/user64/userland/mount/main.o
$(BUILD)/bin/mount: $(AMD64_USER_LIBC_OBJS) $(AMD64_USER_MOUNT_OBJ) \
	$(AMD64_PLATFORM)/user.ld $(AMD64_USER_ELF_CHECK)
	@mkdir -p $(dir $@)
	$(LD) -m elf_x86_64 --gc-sections -nostdlib -static \
		-z max-page-size=4096 -z stack-size=0x100000 \
		-T $(AMD64_PLATFORM)/user.ld $(AMD64_USER_LIBC_OBJS) \
		$(AMD64_USER_MOUNT_OBJ) -o $@
	@test -z "$$(nm -u $@)" || { nm -u $@; exit 1; }
	$(PYTHON) $(AMD64_USER_ELF_CHECK) --machine amd64 $@
$(BUILD)/bin/umount: $(BUILD)/bin/mount
	@mkdir -p $(dir $@)
	cp -f $< $@

$(BUILD)/bin/nettest: $(AMD64_USER_NET_LIBC_OBJS) \
	$(AMD64_USER_NETTEST_OBJS) $(AMD64_PLATFORM)/user.ld \
	$(AMD64_USER_ELF_CHECK)
	@mkdir -p $(dir $@)
	$(LD) -m elf_x86_64 --gc-sections -nostdlib -static \
		-z max-page-size=4096 -z stack-size=0x100000 \
		-T $(AMD64_PLATFORM)/user.ld $(AMD64_USER_NET_LIBC_OBJS) \
		$(AMD64_USER_NETTEST_OBJS) -o $@
	@test -z "$$(nm -u $@)" || { nm -u $@; exit 1; }
	$(PYTHON) $(AMD64_USER_ELF_CHECK) --machine amd64 $@

USER_NET_COMMANDS := ping ifconfig route dhcpcd nslookup host
USER_NET_COMMAND_TARGETS := $(addprefix $(BUILD)/bin/,$(USER_NET_COMMANDS))
AMD64_USER_NET_COMMON_OBJS := $(BUILD)/user64/userland/net/netutil.o \
	$(BUILD)/user64/userland/net/dhcp.o
AMD64_USER_NET_COMMAND_OBJS := $(addsuffix /main.o, \
	$(addprefix $(BUILD)/user64/userland/,$(USER_NET_COMMANDS)))

define AMD64_USER_NET_COMMAND
$(BUILD)/bin/$(1): $(AMD64_USER_NET_LIBC_OBJS) \
	$(AMD64_USER_NET_COMMON_OBJS) $(BUILD)/user64/userland/$(1)/main.o \
	$(AMD64_PLATFORM)/user.ld $(AMD64_USER_ELF_CHECK)
	@mkdir -p $$(dir $$@)
	$(LD) -m elf_x86_64 --gc-sections -nostdlib -static \
		-z max-page-size=4096 -z stack-size=0x100000 \
		-T $(AMD64_PLATFORM)/user.ld $(AMD64_USER_NET_LIBC_OBJS) \
		$(AMD64_USER_NET_COMMON_OBJS) \
		$(BUILD)/user64/userland/$(1)/main.o -o $$@
	@test -z "$$$$(nm -u $$@)" || { nm -u $$@; exit 1; }
	$(PYTHON) $(AMD64_USER_ELF_CHECK) --machine amd64 $$@
endef
$(foreach command,$(USER_NET_COMMANDS),\
	$(eval $(call AMD64_USER_NET_COMMAND,$(command))))
network-tools: $(USER_NET_COMMAND_TARGETS)
.PHONY: network-tools

USER_BASIC_COMMANDS := basename dirname cat mkdir rmdir cp mv rm unlink ln link touch readlink realpath pathchk truncate ls dd more less dmesg chmod chown chgrp mkfifo stat file uname date df du tty stty sleep head tail wc tee cmp cksum od strings tr cut paste sort uniq join comm split csplit fold fmt pr nl expand unexpand grep sed awk xargs iconv diff patch id logname kill nohup time timeout mesg
USER_BASIC_COMMANDS += who login
USER_BASIC_TARGETS := $(addprefix $(BUILD)/bin/,$(USER_BASIC_COMMANDS))
AMD64_USER_BASIC_COMMON_OBJ := $(BUILD)/user64/userland/common/command.o $(BUILD)/user64/userland/common/pager.o

define AMD64_USER_BASIC_COMMAND
$(BUILD)/bin/$(1): $(AMD64_USER_LIBC_OBJS) \
	$(AMD64_USER_BASIC_COMMON_OBJ) $(BUILD)/user64/userland/$(1)/main.o \
	$(AMD64_PLATFORM)/user.ld $(AMD64_USER_ELF_CHECK)
	@mkdir -p $$(dir $$@)
	$(LD) -m elf_x86_64 --gc-sections -nostdlib -static \
		-z max-page-size=4096 -z stack-size=0x100000 \
		-T $(AMD64_PLATFORM)/user.ld $(AMD64_USER_LIBC_OBJS) \
		$(AMD64_USER_BASIC_COMMON_OBJ) \
		$(BUILD)/user64/userland/$(1)/main.o -o $$@
	@test -z "$$$$(nm -u $$@)" || { nm -u $$@; exit 1; }
	$(PYTHON) $(AMD64_USER_ELF_CHECK) --machine amd64 $$@
endef
$(foreach command,$(USER_BASIC_COMMANDS),\
	$(eval $(call AMD64_USER_BASIC_COMMAND,$(command))))
basic-tools: $(USER_BASIC_TARGETS)
.PHONY: basic-tools

# ELF64 runtime linker and shared libc.
DYNAMIC_DIR := $(BUILD)/dynamic
DYNAMIC_CPPFLAGS := -nostdinc -I. -Iinclude -Iinclude/uapi -Ilibc/include \
	-DHAL_ARCH_AMD64 -DZEDBSD_USER_ABI_LP64 -DZEDBSD_DYNAMIC_LIBC
DYNAMIC_CFLAGS := -m64 -march=x86-64 -mno-red-zone -Os -ffreestanding \
	-fPIC -fno-builtin -fno-stack-protector \
	-fno-asynchronous-unwind-tables -fno-unwind-tables \
	-ftls-model=global-dynamic -Wall -Wextra -Werror
DYNAMIC_LIBC_SOURCES := userland/libc/posix.c userland/libc/poll.c \
	userland/libc/termios.c userland/libc/pthread.c userland/libc/shm.c \
	userland/libc/semaphore.c userland/libc/mqueue.c userland/libc/dlfcn.c \
	userland/libc/socket.c userland/libc/resolver.c \
	userland/libc/resolver-dns.c userland/libc/signal.c \
	userland/libc/account.c userland/libc/crypt.c userland/libc/utmpx.c libc/heap.c \
	libc/string.c libc/ctype.c libc/locale.c libc/wide.c libc/int64.c \
	libc/strto.c libc/format.c \
	libc/stdio.c
DYNAMIC_LIBC_OBJS := $(patsubst %.c,$(DYNAMIC_DIR)/obj/%.o,\
	$(DYNAMIC_LIBC_SOURCES)) $(DYNAMIC_DIR)/obj/userland/libc/syscall.o
DYNAMIC_RTLD_OBJS := $(DYNAMIC_DIR)/obj/userland/rtld/entry.o \
	$(DYNAMIC_DIR)/obj/userland/rtld/tlsdesc.o \
	$(DYNAMIC_DIR)/obj/userland/rtld/rtld.o \
	$(DYNAMIC_DIR)/obj/userland/rtld/string.o
DYNAMIC_FLOAT_DIR := $(DYNAMIC_DIR)/float
DYNAMIC_MUSL_MATH_OBJS := $(addprefix $(DYNAMIC_FLOAT_DIR)/musl-,\
	$(ZEDBSD_MUSL_MATH_REL:.c=.o))
DYNAMIC_MUSL_SCAN_OBJS := $(DYNAMIC_FLOAT_DIR)/musl-shgetc.o \
	$(DYNAMIC_FLOAT_DIR)/musl-floatscan.o \
	$(DYNAMIC_FLOAT_DIR)/musl-strtod.o \
	$(DYNAMIC_FLOAT_DIR)/musl-compat.o
DYNAMIC_LIBC_OBJS += $(DYNAMIC_MUSL_MATH_OBJS) $(DYNAMIC_MUSL_SCAN_OBJS)

$(DYNAMIC_DIR)/obj/%.o: %.c
	@mkdir -p $(dir $@)
	$(CC) $(DYNAMIC_CPPFLAGS) $(DYNAMIC_CFLAGS) -MMD -MP -c $< -o $@

$(DYNAMIC_DIR)/obj/userland/libc/syscall.o: userland/libc/syscall-amd64.S
	@mkdir -p $(dir $@)
	$(CC) -m64 -c $< -o $@

$(DYNAMIC_DIR)/obj/userland/rtld/entry.o: userland/rtld/entry-amd64.S
	@mkdir -p $(dir $@)
	$(CC) -m64 -c $< -o $@

$(DYNAMIC_DIR)/obj/userland/rtld/tlsdesc.o: userland/rtld/tlsdesc-amd64.S
	@mkdir -p $(dir $@)
	$(CC) -m64 -c $< -o $@

$(DYNAMIC_DIR)/obj/userland/tests/tlstest.o: DYNAMIC_CFLAGS += -mtls-dialect=gnu2

$(DYNAMIC_FLOAT_DIR)/musl-%.o: $(ZEDBSD_MUSL_ROOT)/src/math/%.c
	@mkdir -p $(dir $@)
	$(CC) $(ZEDBSD_MUSL_CPPFLAGS) $(DYNAMIC_CFLAGS) -mlong-double-64 \
		-Wno-error=unused-but-set-variable -Wno-error=parentheses \
		-c $< -o $@

$(DYNAMIC_FLOAT_DIR)/musl-shgetc.o: \
	$(ZEDBSD_MUSL_ROOT)/src/internal/shgetc.c src/softfloat/musl-floatscan.h
	@mkdir -p $(dir $@)
	$(CC) $(ZEDBSD_MUSL_CPPFLAGS) $(DYNAMIC_CFLAGS) -mlong-double-64 \
		-Wno-error=parentheses -include src/softfloat/musl-floatscan.h \
		-c $< -o $@

$(DYNAMIC_FLOAT_DIR)/musl-floatscan.o: \
	$(ZEDBSD_MUSL_ROOT)/src/internal/floatscan.c src/softfloat/musl-floatscan.h
	@mkdir -p $(dir $@)
	$(CC) $(ZEDBSD_MUSL_CPPFLAGS) $(DYNAMIC_CFLAGS) -mlong-double-64 \
		-Wno-error=parentheses -Wno-error=sign-compare \
		-include src/softfloat/musl-floatscan.h -c $< -o $@

$(DYNAMIC_FLOAT_DIR)/musl-strtod.o: \
	$(ZEDBSD_MUSL_ROOT)/src/stdlib/strtod.c src/softfloat/musl-floatscan.h
	@mkdir -p $(dir $@)
	$(CC) $(ZEDBSD_MUSL_CPPFLAGS) $(DYNAMIC_CFLAGS) -mlong-double-64 \
		-include src/softfloat/musl-floatscan.h -c $< -o $@

$(DYNAMIC_FLOAT_DIR)/musl-compat.o: src/softfloat/musl-compat.c \
	src/softfloat/musl-floatscan.h
	@mkdir -p $(dir $@)
	$(CC) $(ZEDBSD_MUSL_CPPFLAGS) $(DYNAMIC_CFLAGS) -mlong-double-64 \
		-include src/softfloat/musl-floatscan.h -c $< -o $@

$(DYNAMIC_DIR)/obj/userland/crt1.o: userland/crt1-amd64.S
	@mkdir -p $(dir $@)
	$(CC) -m64 -c $< -o $@

$(DYNAMIC_DIR)/ld.so: $(DYNAMIC_RTLD_OBJS)
	$(LD) -m elf_x86_64 -shared -Bsymbolic -e _rtld_start \
		--hash-style=sysv -z now -z relro -z separate-code $^ -o $@

$(DYNAMIC_DIR)/libc.so: $(DYNAMIC_LIBC_OBJS)
	$(LD) -m elf_x86_64 -shared -soname libc.so --hash-style=both \
		-z now -z relro -z separate-code -z stack-size=0x100000 $^ -o $@

$(DYNAMIC_DIR)/alt/rpathdep.so: \
	$(DYNAMIC_DIR)/obj/userland/tests/rpathdep.o $(DYNAMIC_DIR)/ld.so
	@mkdir -p $(dir $@)
	$(LD) -m elf_x86_64 -shared -soname rpathdep.so --hash-style=gnu \
		-z now -z relro -z separate-code $< -o $@

$(DYNAMIC_DIR)/tlstest.so: \
	$(DYNAMIC_DIR)/obj/userland/tests/tlstest.o \
	$(DYNAMIC_DIR)/alt/rpathdep.so $(DYNAMIC_DIR)/ld.so
	$(LD) -m elf_x86_64 -shared -soname tlstest.so --hash-style=gnu \
		-z now -z relro -z separate-code --enable-new-dtags \
		-rpath '$$ORIGIN/alt' \
		$(DYNAMIC_DIR)/obj/userland/tests/tlstest.o \
		-L$(DYNAMIC_DIR)/alt -l:rpathdep.so -o $@

$(DYNAMIC_DIR)/rpathtest.so: \
	$(DYNAMIC_DIR)/obj/userland/tests/rpathtest.o \
	$(DYNAMIC_DIR)/alt/rpathdep.so $(DYNAMIC_DIR)/ld.so
	$(LD) -m elf_x86_64 -shared -soname rpthtest.so --hash-style=gnu \
		-z now -z relro -z separate-code --disable-new-dtags \
		-rpath '$$ORIGIN/alt' $< -L$(DYNAMIC_DIR)/alt \
		-l:rpathdep.so -o $@

$(DYNAMIC_DIR)/verstest.so: \
	$(DYNAMIC_DIR)/obj/userland/tests/versiontest.o \
	userland/tests/versiontest.map $(DYNAMIC_DIR)/ld.so
	$(LD) -m elf_x86_64 -shared -soname verstest.so --hash-style=gnu \
		-z now -z relro -z separate-code \
		--version-script=userland/tests/versiontest.map $< -o $@

$(DYNAMIC_DIR)/versuse.so: \
	$(DYNAMIC_DIR)/obj/userland/tests/versionuse.o \
	$(DYNAMIC_DIR)/verstest.so $(DYNAMIC_DIR)/ld.so
	$(LD) -m elf_x86_64 -shared -soname versuse.so --hash-style=gnu \
		-z now -z relro -z separate-code $< -L$(DYNAMIC_DIR) \
		-l:verstest.so -o $@

$(DYNAMIC_DIR)/dyntest: $(DYNAMIC_DIR)/obj/userland/crt1.o \
	$(DYNAMIC_DIR)/obj/userland/tests/dyntest.o $(DYNAMIC_DIR)/libc.so \
	$(DYNAMIC_DIR)/ld.so $(DYNAMIC_DIR)/tlstest.so \
	$(DYNAMIC_DIR)/versuse.so
	$(CC) -m64 -nostdlib -pie -Wl,--no-relax \
		-Wl,--hash-style=sysv,-z,now,-z,relro,-z,separate-code \
		-Wl,-z,stack-size=0x100000,--allow-shlib-undefined \
		-Wl,--dynamic-linker=/lib/ld.so \
		$(DYNAMIC_DIR)/obj/userland/crt1.o \
		$(DYNAMIC_DIR)/obj/userland/tests/dyntest.o \
		-L$(DYNAMIC_DIR) -Wl,-rpath-link,$(DYNAMIC_DIR) \
		-l:libc.so -o $@

dynamic-userland-check: $(DYNAMIC_DIR)/ld.so $(DYNAMIC_DIR)/libc.so \
	$(DYNAMIC_DIR)/dyntest $(DYNAMIC_DIR)/tlstest.so \
	$(DYNAMIC_DIR)/rpathtest.so $(DYNAMIC_DIR)/verstest.so \
	$(DYNAMIC_DIR)/versuse.so scripts/check-dynamic-elf.py
	$(PYTHON) scripts/check-dynamic-elf.py --machine amd64 --role interpreter $(DYNAMIC_DIR)/ld.so
	$(PYTHON) scripts/check-dynamic-elf.py --machine amd64 --role libc $(DYNAMIC_DIR)/libc.so
	$(PYTHON) scripts/check-dynamic-elf.py --machine amd64 --role module $(DYNAMIC_DIR)/tlstest.so
	$(PYTHON) scripts/check-dynamic-elf.py --machine amd64 --role rpath-module $(DYNAMIC_DIR)/rpathtest.so
	$(PYTHON) scripts/check-dynamic-elf.py --machine amd64 --role version-definition $(DYNAMIC_DIR)/verstest.so
	$(PYTHON) scripts/check-dynamic-elf.py --machine amd64 --role version-consumer $(DYNAMIC_DIR)/versuse.so
	$(PYTHON) scripts/check-dynamic-elf.py --machine amd64 --role program $(DYNAMIC_DIR)/dyntest
	@echo "zedBSD amd64 dynamic userland artifacts: PASS"
.PHONY: dynamic-userland-check

AMD64_ARCH_IMAGE := $(ARCH_IMAGE_DIR)/amd64.img
AMD64_ARCH_INPUTS := $(BUILD)/bin/sh $(BUILD)/bin/nettest \
	$(BUILD)/bin/ping $(BUILD)/bin/ifconfig $(BUILD)/bin/route \
	$(BUILD)/bin/dhcpcd $(BUILD)/bin/nslookup $(BUILD)/bin/host $(BUILD)/bin/sysctl \
	$(BUILD)/bin/mount $(BUILD)/bin/umount \
	$(DYNAMIC_DIR)/ld.so $(DYNAMIC_DIR)/libc.so \
	$(DYNAMIC_DIR)/tlstest.so $(DYNAMIC_DIR)/dyntest \
	$(DYNAMIC_DIR)/alt/rpathdep.so $(DYNAMIC_DIR)/rpathtest.so \
	$(DYNAMIC_DIR)/verstest.so $(DYNAMIC_DIR)/versuse.so
AMD64_ARCH_FILES := --file /bin/sh=$(BUILD)/bin/sh \
	--file /bin/nettest=$(BUILD)/bin/nettest \
	--file /bin/ping=$(BUILD)/bin/ping \
	--file /bin/ifconfig=$(BUILD)/bin/ifconfig \
	--file /bin/route=$(BUILD)/bin/route \
	--file /bin/dhcpcd=$(BUILD)/bin/dhcpcd \
	--file /bin/nslookup=$(BUILD)/bin/nslookup \
	--file /bin/host=$(BUILD)/bin/host \
	--file /bin/sysctl=$(BUILD)/bin/sysctl \
	--file /bin/mount=$(BUILD)/bin/mount \
	--file /bin/umount=$(BUILD)/bin/umount \
	--file /lib/ld.so=$(DYNAMIC_DIR)/ld.so \
	--file /lib/libc.so=$(DYNAMIC_DIR)/libc.so \
	--file /lib/tlstest.so=$(DYNAMIC_DIR)/tlstest.so \
	--file /lib/alt/rpathdep.so=$(DYNAMIC_DIR)/alt/rpathdep.so \
	--file /lib/rpthtest.so=$(DYNAMIC_DIR)/rpathtest.so \
	--file /lib/verstest.so=$(DYNAMIC_DIR)/verstest.so \
	--file /lib/versuse.so=$(DYNAMIC_DIR)/versuse.so \
	--file /bin/dyntest=$(DYNAMIC_DIR)/dyntest
AMD64_ARCH_INPUTS += $(USER_BASIC_TARGETS)
AMD64_ARCH_FILES += $(foreach command,$(USER_BASIC_COMMANDS),--file /bin/$(command)=$(BUILD)/bin/$(command))
AMD64_ARCH_INPUTS += $(ZEDBSD_ACCOUNT_INPUTS)
AMD64_ARCH_FILES += $(ZEDBSD_ACCOUNT_FILES)
$(eval $(call ZEDBSD_ARCH_IMAGE_RULE,$(AMD64_ARCH_IMAGE),amd64,$(AMD64_ARCH_INPUTS),$(AMD64_ARCH_FILES)))
$(eval $(call ZEDBSD_ROOTFS_TAR_RULE,$(BUILD)/rootfs.tar.gz,$(AMD64_ARCH_INPUTS),$(AMD64_ARCH_FILES)))
AMD64_ARCH_UFS_IMAGE := $(ARCH_IMAGE_DIR)/amd64.ufs
$(eval $(call ZEDBSD_ARCH_UFS_IMAGE_RULE,$(AMD64_ARCH_UFS_IMAGE),amd64,$(AMD64_ARCH_INPUTS),$(AMD64_ARCH_FILES)))
arch-image: $(AMD64_ARCH_IMAGE)
arch-image-check: $(AMD64_ARCH_IMAGE)-check
arch-image-ufs: $(AMD64_ARCH_UFS_IMAGE)
arch-image-ufs-check: $(AMD64_ARCH_UFS_IMAGE)-check
rootfs-tar: $(BUILD)/rootfs.tar.gz

$(BUILD)/bios-hdd-image.img: $(BUILD)/bootloader/stage1.bin \
	$(BUILD)/bootloader/stage2.bin $(BUILD)/vmunix $(AMD64_ARCH_IMAGE) \
	scripts/make-bios-hdd-image.py scripts/check-bios-hdd-image.py
	$(PYTHON) scripts/make-bios-hdd-image.py --force --machine pcat \
		--stage1 $(BUILD)/bootloader/stage1.bin \
		--stage2 $(BUILD)/bootloader/stage2.bin --kernel $(BUILD)/vmunix \
		--arch-profile amd64 --arch-image $(AMD64_ARCH_IMAGE) $@

$(BUILD)/ufs-root.img: $(AMD64_ARCH_UFS_IMAGE) \
	$(SCRIPTS_DIR)/make-ufs1-root-image.py scripts/ufs1_format.py
	$(PYTHON) $(SCRIPTS_DIR)/make-ufs1-root-image.py --force \
		--arch-profile amd64 --arch-image $(AMD64_ARCH_UFS_IMAGE) $@

$(BUILD)/ufs-root-hdd-image.img: $(BUILD)/bootloader/stage1.bin \
	$(BUILD)/bootloader/stage2.bin $(BUILD)/vmunix $(BUILD)/ufs-root.img \
	$(SCRIPTS_DIR)/make-bios-hdd-image.py
	$(PYTHON) $(SCRIPTS_DIR)/make-bios-hdd-image.py --force \
		--machine pcat --stage1 $(BUILD)/bootloader/stage1.bin \
		--stage2 $(BUILD)/bootloader/stage2.bin --kernel $(BUILD)/vmunix \
		--ufs-root $(BUILD)/ufs-root.img --size-mib 193 $@

ufs-root-image: $(BUILD)/ufs-root-hdd-image.img

$(BUILD)/bios-hdd-image-fragmented.img: $(BUILD)/bootloader/stage1.bin \
	$(BUILD)/bootloader/stage2.bin $(BUILD)/vmunix $(AMD64_ARCH_IMAGE) \
	scripts/make-bios-hdd-image.py scripts/check-bios-hdd-image.py
	$(PYTHON) scripts/make-bios-hdd-image.py --force --machine pcat \
		--stage1 $(BUILD)/bootloader/stage1.bin \
		--stage2 $(BUILD)/bootloader/stage2.bin --kernel $(BUILD)/vmunix \
		--arch-profile amd64 --arch-image $(AMD64_ARCH_IMAGE) \
		--fragment-kernel $@

$(BUILD)/hdd-image.img: $(BUILD)/bios-hdd-image.img
	cp -f $< $@

bios-hdd-image: $(BUILD)/bios-hdd-image.img
hdd-image: $(BUILD)/hdd-image.img
bios-loader-host-check: $(BUILD)/bios-hdd-image.img
	$(PYTHON) scripts/check-bios-hdd-image.py --machine pcat \
		--kernel $(BUILD)/vmunix --arch-profile amd64 \
		--arch-image $(AMD64_ARCH_IMAGE) $<

amd64-hal-compile: $(AMD64_HAL_OBJS)
	@echo "HAL amd64/PCAT compile check: PASS"
CHECK_RUN_TARGETS += amd64-hal-compile

amd64-entry-qemu-test: $(BUILD)/vmunix bios-bootloader
	bash scripts/test-amd64-entry-qemu.sh

hdd-boot-qemu-test amd64-qemu-test: $(BUILD)/hdd-image.img \
	$(BUILD)/bios-hdd-image-fragmented.img
	bash scripts/test-amd64-qemu.sh $(BUILD)/hdd-image.img \
		$(BUILD)/bios-hdd-image-fragmented.img

network-qemu-test: bios-bootloader $(BUILD)/vmunix \
	$(BUILD)/bin/nettest scripts/test-pcat-ne2000.sh
	bash scripts/test-pcat-ne2000.sh amd64

.PHONY: all vmunix SH arch-image arch-image-check arch-image-ufs \
	arch-image-ufs-check ufs-root-image bios-bootloader bios-hdd-image hdd-image \
	bios-loader-host-check amd64-hal-compile amd64-entry-qemu-test \
	hdd-boot-qemu-test amd64-qemu-test network-qemu-test

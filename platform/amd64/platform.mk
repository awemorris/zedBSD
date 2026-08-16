# zedBSD amd64/PC-AT bootstrap rules.
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib

AMD64_PLATFORM := platform/amd64
BIOS_LOADER := bootloader/pcat

AMD64_CPPFLAGS := -nostdinc -Iinclude -Iinclude/uapi -Isrc -I. \
	-Ilibc/include -DHAL_ARCH_AMD64 -DHAL_BOARD_PCAT -DHAL_PCAT_DEBUGCON \
	-DZEDBSD_USER_ABI_LP64 \
	-DPCAT_VGA_APERTURE_ADDRESS=0xffffffff800a0000ULL \
	-DPCAT_CIRRUS_APERTURE_ADDRESS=0xffffffffc0000000ULL
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
	src/kern/rootfs.c src/kern/overlayfs.c src/kern/vfs.c \
	src/kern/swap.c src/kern/swap-fat.c \
	src/kern/vm-reclaim.c src/kern/buf.c src/kern/sysctl.c \
	src/kern/resource.c \
	src/kern/disk.c src/kern/partition.c \
	drivers/loop.c \
	drivers/pcat-ide.c drivers/dp8390.c drivers/pcat-ne2000.c \
	src/kern/mbr-partition.c src/kern/pcat/platform.c \
	src/kern/image.c src/kern/panic.c src/kern/entry.c src/kern/clock.c \
	src/kern/lock.c src/kern/waitq.c \
	src/kern/process.c src/kern/thread.c src/kern/sched.c \
	src/kern/vmspace.c src/kern/vm-object.c src/kern/vm-commit.c \
	src/kern/filedesc.c \
	src/kern/pipe.c src/kern/cred.c src/kern/signal.c \
	src/kern/cwdinfo.c src/kern/elf.c src/kern/exec.c \
	src/kern/user-probe.c src/kern/syscall.c src/kern/uaccess.c \
	src/kern/cdev.c src/kern/devfs.c src/kern/console-device.c \
	src/kern/graphics-device.c src/kern/system-device.c \
	src/kern/pcat/font.c drivers/pcat-graphics.c \
	src/kern/init.c
AMD64_KERNEL_SOURCES += $(KERN_NET_SOURCES) $(KERN_UFS1_SOURCES)
AMD64_KERNEL_OBJS := $(patsubst %.c,$(BUILD)/kern64/%.o,\
	$(AMD64_KERNEL_SOURCES))
AMD64_KERNEL_LIBC_OBJS := $(patsubst %.c,$(BUILD)/kern64/%.o,\
	$(ZEDBSD_LIBC_SOURCES))
AMD64_VMUNIX_OBJS := $(AMD64_HAL_OBJS) $(AMD64_KERNEL_OBJS) \
	$(AMD64_KERNEL_LIBC_OBJS)

all: $(BUILD)/vmunix $(BUILD)/bin/sh $(BUILD)/bin/nettest \
	$(BUILD)/bin/ping $(BUILD)/bin/ifconfig $(BUILD)/bin/route \
	$(BUILD)/bin/dhcpcd $(BUILD)/bin/nslookup $(BUILD)/bin/sysctl \
	$(BUILD)/hdd-image.img
vmunix: $(BUILD)/vmunix
SH: $(BUILD)/bin/sh
POSIX-R1.ELF: $(BUILD)/POSIX-R1.ELF
SMP-STRESS.ELF: $(BUILD)/SMP-STRESS.ELF
.PHONY: POSIX-R1.ELF SMP-STRESS.ELF

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
AMD64_USER_RUNTIME_SOURCES := userland/libc/posix.c \
	userland/libc/signal.c libc/heap.c libc/string.c libc/ctype.c \
	libc/int64.c libc/strto.c libc/format.c libc/stdio.c
AMD64_USER_LIBC_OBJS := $(BUILD)/user64/userland/crt0-amd64.o \
	$(patsubst %.c,$(BUILD)/user64/%.o,$(AMD64_USER_RUNTIME_SOURCES))
AMD64_USER_NET_LIBC_OBJS := $(AMD64_USER_LIBC_OBJS) \
	$(BUILD)/user64/userland/libc/socket.o \
	$(BUILD)/user64/userland/libc/resolver.o \
	$(BUILD)/user64/userland/libc/resolver-dns.o
AMD64_USER_NETTEST_OBJS := $(BUILD)/user64/userland/nettest/main.o
AMD64_USER_SH_OBJS := $(BUILD)/user64/userland/sh/main.o \
	$(BUILD)/user64/userland/sh/applet.o \
	$(BUILD)/user64/userland/sh/builtins.o
AMD64_USER_ELF_CHECK := scripts/check-user-elf.py

$(BUILD)/user64/%.o: %.c
	@mkdir -p $(dir $@)
	$(CC) $(AMD64_USER_CPPFLAGS) $(AMD64_USER_CFLAGS) \
		-fno-strict-aliasing -MMD -MP -c $< -o $@

$(BUILD)/user64/userland/crt0-amd64.o: userland/crt0-amd64.S
	@mkdir -p $(dir $@)
	$(CC) $(AMD64_USER_CPPFLAGS) $(AMD64_USER_CFLAGS) -c $< -o $@

$(BUILD)/POSIX-R1.ELF: $(AMD64_USER_LIBC_OBJS) \
	$(BUILD)/user64/userland/tests/syscall-smoke.o $(AMD64_PLATFORM)/user.ld \
	$(AMD64_USER_ELF_CHECK)
	$(LD) -m elf_x86_64 --gc-sections -nostdlib -static \
		-z max-page-size=4096 -z stack-size=0x100000 \
		-T $(AMD64_PLATFORM)/user.ld \
		$(AMD64_USER_LIBC_OBJS) \
		$(BUILD)/user64/userland/tests/syscall-smoke.o -o $@
	$(PYTHON) $(AMD64_USER_ELF_CHECK) --machine amd64 $@

$(BUILD)/bin/sh: $(AMD64_USER_LIBC_OBJS) $(AMD64_USER_SH_OBJS) \
	$(AMD64_PLATFORM)/user.ld \
	$(AMD64_USER_ELF_CHECK)
	@mkdir -p $(dir $@)
	$(LD) -m elf_x86_64 --gc-sections -nostdlib -static \
		-z max-page-size=4096 -z stack-size=0x100000 \
		-T $(AMD64_PLATFORM)/user.ld $(AMD64_USER_LIBC_OBJS) \
		$(AMD64_USER_SH_OBJS) -o $@
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

USER_NET_COMMANDS := ping ifconfig route dhcpcd nslookup
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

AMD64_ARCH_IMAGE := $(ARCH_IMAGE_DIR)/amd64.img
AMD64_ARCH_INPUTS := $(BUILD)/bin/sh $(BUILD)/bin/nettest \
	$(BUILD)/bin/ping $(BUILD)/bin/ifconfig $(BUILD)/bin/route \
	$(BUILD)/bin/dhcpcd $(BUILD)/bin/nslookup $(BUILD)/bin/sysctl
AMD64_ARCH_FILES := --file /bin/sh=$(BUILD)/bin/sh \
	--file /bin/nettest=$(BUILD)/bin/nettest \
	--file /bin/ping=$(BUILD)/bin/ping \
	--file /bin/ifconfig=$(BUILD)/bin/ifconfig \
	--file /bin/route=$(BUILD)/bin/route \
	--file /bin/dhcpcd=$(BUILD)/bin/dhcpcd \
	--file /bin/nslookup=$(BUILD)/bin/nslookup \
	--file /bin/sysctl=$(BUILD)/bin/sysctl
$(eval $(call ZEDBSD_ARCH_IMAGE_RULE,$(AMD64_ARCH_IMAGE),amd64,$(AMD64_ARCH_INPUTS),$(AMD64_ARCH_FILES)))
AMD64_ARCH_UFS_IMAGE := $(ARCH_IMAGE_DIR)/amd64.ufs
$(eval $(call ZEDBSD_ARCH_UFS_IMAGE_RULE,$(AMD64_ARCH_UFS_IMAGE),amd64,$(AMD64_ARCH_INPUTS),$(AMD64_ARCH_FILES)))
arch-image: $(AMD64_ARCH_IMAGE)
arch-image-check: $(AMD64_ARCH_IMAGE)-check
arch-image-ufs: $(AMD64_ARCH_UFS_IMAGE)
arch-image-ufs-check: $(AMD64_ARCH_UFS_IMAGE)-check

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

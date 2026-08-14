# zedBSD amd64/PC-AT bootstrap rules.
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib

AMD64_PLATFORM := platform/amd64
BIOS_LOADER := bootloader/pcat

AMD64_CPPFLAGS := -nostdinc -Iinclude -Iinclude/uapi -Isrc -I. \
	-Ilibc/include -DHAL_ARCH_AMD64 -DHAL_BOARD_PCAT -DHAL_PCAT_DEBUGCON \
	-DPCAT_VGA_APERTURE_ADDRESS=0xffffffff800a0000ULL \
	-DPCAT_CIRRUS_APERTURE_ADDRESS=0xffffffffc0000000ULL
AMD64_CFLAGS := -m64 -mcmodel=kernel -mno-red-zone -mgeneral-regs-only \
	-ffreestanding -fno-pic -fno-pie -fno-stack-protector \
	-fno-asynchronous-unwind-tables -fno-unwind-tables \
	-Os -Wall -Wextra -Werror
AMD64_KERNEL_LIBC_CFLAGS := $(filter-out -mgeneral-regs-only,$(AMD64_CFLAGS))

AMD64_HAL_SOURCES := src/hal/amd64/asm.c src/hal/amd64/lib.c \
	src/hal/amd64/page.c src/hal/amd64/space.c src/hal/amd64/cmain.c \
	src/hal/amd64/descriptor.c src/hal/amd64/int.c src/hal/amd64/irq.c \
	src/hal/amd64/task.c src/hal/amd64/fb.c \
	src/hal/amd64/bsp-pcat/boot.c src/hal/amd64/bsp-pcat/cons.c \
	src/hal/amd64/bsp-pcat/pic.c src/hal/amd64/bsp-pcat/clock.c
AMD64_HAL_ASM := src/hal/amd64/locore.S src/hal/amd64/trap.S \
	src/hal/amd64/dispatch.S
AMD64_HAL_OBJS := $(patsubst %.c,$(BUILD)/%.o,$(AMD64_HAL_SOURCES)) \
	$(patsubst %.S,$(BUILD)/%.o,$(AMD64_HAL_ASM))

AMD64_KERNEL_SOURCES := \
	src/kern/main.c src/kern/env.c src/kern/fs.c src/kern/namespace.c \
	src/kern/fat.c src/kern/fat-lfn.c src/kern/fat16.c \
	src/kern/fat-vfs.c src/kern/inode.c src/kern/file.c \
	src/kern/namecache.c src/kern/namei.c src/kern/mount.c \
	src/kern/rootfs.c src/kern/vfs.c src/kern/swap.c src/kern/swap-fat.c \
	src/kern/vm-reclaim.c src/kern/disk.c src/kern/partition.c \
	drivers/pcat-ide.c drivers/dp8390.c drivers/pcat-ne2000.c \
	src/kern/mbr-partition.c src/kern/pcat/platform.c \
	src/kern/image.c src/kern/panic.c src/kern/entry.c src/kern/clock.c \
	src/kern/process.c src/kern/thread.c src/kern/sched.c \
	src/kern/vmspace.c src/kern/vm-commit.c src/kern/filedesc.c \
	src/kern/cwdinfo.c src/kern/elf.c src/kern/exec.c \
	src/kern/user-probe.c src/kern/syscall.c src/kern/uaccess.c \
	src/kern/cdev.c src/kern/devfs.c src/kern/console-device.c \
	src/kern/graphics-device.c src/kern/system-device.c \
	src/kern/pcat/font.c src/kern/pcat/graphics.c \
	src/kern/pcat/unsupported-devices.c src/kern/init.c
AMD64_KERNEL_SOURCES += $(KERN_NET_SOURCES)
AMD64_KERNEL_OBJS := $(patsubst %.c,$(BUILD)/kern64/%.o,\
	$(AMD64_KERNEL_SOURCES))
AMD64_KERNEL_LIBC_OBJS := $(patsubst %.c,$(BUILD)/kern64/%.o,\
	$(ZEDBSD_LIBC_SOURCES))
AMD64_VMUNIX_OBJS := $(AMD64_HAL_OBJS) $(AMD64_KERNEL_OBJS) \
	$(AMD64_KERNEL_LIBC_OBJS)

all: $(BUILD)/vmunix $(BUILD)/bin/sh $(BUILD)/bin/nettest \
	$(BUILD)/bin/ping $(BUILD)/bin/ifconfig $(BUILD)/bin/route \
	$(BUILD)/bin/dhcpcd $(BUILD)/bin/nslookup \
	$(BUILD)/hdd-image.img
vmunix: $(BUILD)/vmunix
SH: $(BUILD)/bin/sh

$(BUILD)/src/hal/amd64/%.o: src/hal/amd64/%.S
	@mkdir -p $(dir $@)
	$(CC) $(AMD64_CPPFLAGS) $(AMD64_CFLAGS) -D_ASM_SRC_ -c $< -o $@

$(BUILD)/src/hal/amd64/%.o: src/hal/amd64/%.c
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

AMD64_USER_LIBC_OBJS := $(BUILD)/userland/crt0.o \
	$(BUILD)/userland/libc/posix.o $(BUILD)/libc/heap.o \
	$(BUILD)/libc/string.o $(BUILD)/libc/ctype.o $(BUILD)/libc/int64.o \
	$(BUILD)/libc/strto.o $(BUILD)/libc/format.o $(BUILD)/libc/stdio.o
AMD64_USER_NET_LIBC_OBJS := $(AMD64_USER_LIBC_OBJS) \
	$(BUILD)/userland/libc/socket.o $(BUILD)/userland/libc/resolver.o \
	$(BUILD)/userland/libc/resolver-dns.o
AMD64_USER_NETTEST_OBJS := $(BUILD)/userland/nettest/main.o
AMD64_USER_CFLAGS := $(ZEDBSD_CFLAGS) -fno-builtin -ffunction-sections \
	-fdata-sections -msoft-float -mno-80387 -mno-fp-ret-in-387 \
	-mno-mmx -mno-sse -mno-sse2
AMD64_USER_SH_OBJS := $(BUILD)/userland/sh/main.o \
	$(BUILD)/userland/sh/applet.o $(BUILD)/userland/sh/builtins.o
AMD64_USER_ELF_CHECK := scripts/check-user-elf.py

$(BUILD)/userland/libc/posix.o $(AMD64_USER_SH_OBJS): \
	OBJ_CPPFLAGS = $(ZEDBSD_CPPFLAGS)
$(BUILD)/userland/libc/posix.o $(AMD64_USER_SH_OBJS): \
	OBJ_CFLAGS = $(AMD64_USER_CFLAGS)

$(BUILD)/userland/libc/socket.o $(AMD64_USER_NETTEST_OBJS): \
	OBJ_CPPFLAGS = $(ZEDBSD_CPPFLAGS)
$(BUILD)/userland/libc/socket.o $(AMD64_USER_NETTEST_OBJS): \
	OBJ_CFLAGS = $(AMD64_USER_CFLAGS)

$(BUILD)/bin/sh: $(AMD64_USER_LIBC_OBJS) $(AMD64_USER_SH_OBJS) \
	$(ZEDBSD_SOFTFLOAT_OBJECTS) platform/pcat/user.ld \
	$(AMD64_USER_ELF_CHECK)
	@mkdir -p $(dir $@)
	$(LD) -m elf_i386 --gc-sections -nostdlib -static \
		-z max-page-size=4096 -z stack-size=0x100000 \
		-T platform/pcat/user.ld $(AMD64_USER_LIBC_OBJS) \
		$(AMD64_USER_SH_OBJS) $(ZEDBSD_SOFTFLOAT_OBJECTS) -o $@
	@test -z "$$($(NOCT_NM) -u $@)" || { $(NOCT_NM) -u $@; exit 1; }
	$(PYTHON) $(AMD64_USER_ELF_CHECK) $@

$(BUILD)/bin/nettest: $(AMD64_USER_NET_LIBC_OBJS) \
	$(AMD64_USER_NETTEST_OBJS) $(ZEDBSD_SOFTFLOAT_OBJECTS) \
	platform/pcat/user.ld $(AMD64_USER_ELF_CHECK)
	@mkdir -p $(dir $@)
	$(LD) -m elf_i386 --gc-sections -nostdlib -static \
		-z max-page-size=4096 -z stack-size=0x100000 \
		-T platform/pcat/user.ld $(AMD64_USER_NET_LIBC_OBJS) \
		$(AMD64_USER_NETTEST_OBJS) $(ZEDBSD_SOFTFLOAT_OBJECTS) -o $@
	@test -z "$$($(NOCT_NM) -u $@)" || { $(NOCT_NM) -u $@; exit 1; }
	$(PYTHON) $(AMD64_USER_ELF_CHECK) $@

USER_NET_COMMANDS := ping ifconfig route dhcpcd nslookup
USER_NET_COMMAND_TARGETS := $(addprefix $(BUILD)/bin/,$(USER_NET_COMMANDS))
AMD64_USER_NET_COMMON_OBJS := $(BUILD)/userland/net/netutil.o \
	$(BUILD)/userland/net/dhcp.o
AMD64_USER_NET_COMMAND_OBJS := $(addsuffix /main.o, \
	$(addprefix $(BUILD)/userland/,$(USER_NET_COMMANDS)))
$(BUILD)/userland/libc/socket.o $(BUILD)/userland/libc/resolver.o \
	$(BUILD)/userland/libc/resolver-dns.o $(AMD64_USER_NET_COMMON_OBJS) \
	$(AMD64_USER_NET_COMMAND_OBJS): OBJ_CPPFLAGS = $(ZEDBSD_CPPFLAGS)
$(BUILD)/userland/libc/socket.o $(BUILD)/userland/libc/resolver.o \
	$(BUILD)/userland/libc/resolver-dns.o $(AMD64_USER_NET_COMMON_OBJS) \
	$(AMD64_USER_NET_COMMAND_OBJS): OBJ_CFLAGS = $(AMD64_USER_CFLAGS)

define AMD64_USER_NET_COMMAND
$(BUILD)/bin/$(1): $(AMD64_USER_NET_LIBC_OBJS) \
	$(AMD64_USER_NET_COMMON_OBJS) $(BUILD)/userland/$(1)/main.o \
	$(ZEDBSD_SOFTFLOAT_OBJECTS) platform/pcat/user.ld $(AMD64_USER_ELF_CHECK)
	@mkdir -p $$(dir $$@)
	$(LD) -m elf_i386 --gc-sections -nostdlib -static \
		-z max-page-size=4096 -z stack-size=0x100000 \
		-T platform/pcat/user.ld $(AMD64_USER_NET_LIBC_OBJS) \
		$(AMD64_USER_NET_COMMON_OBJS) $(BUILD)/userland/$(1)/main.o \
		$(ZEDBSD_SOFTFLOAT_OBJECTS) -o $$@
	@test -z "$$$$($(NOCT_NM) -u $$@)" || { $(NOCT_NM) -u $$@; exit 1; }
	$(PYTHON) $(AMD64_USER_ELF_CHECK) $$@
endef
$(foreach command,$(USER_NET_COMMANDS),\
	$(eval $(call AMD64_USER_NET_COMMAND,$(command))))
network-tools: $(USER_NET_COMMAND_TARGETS)
.PHONY: network-tools

$(BUILD)/bios-hdd-image.img: $(BUILD)/bootloader/stage1.bin \
	$(BUILD)/bootloader/stage2.bin $(BUILD)/vmunix $(BUILD)/bin/sh \
	$(BUILD)/bin/nettest \
	$(BUILD)/bin/ping $(BUILD)/bin/ifconfig $(BUILD)/bin/route \
	$(BUILD)/bin/dhcpcd $(BUILD)/bin/nslookup \
	scripts/make-bios-hdd-image.py scripts/check-bios-hdd-image.py
	$(PYTHON) scripts/make-bios-hdd-image.py --force --machine pcat \
		--stage1 $(BUILD)/bootloader/stage1.bin \
		--stage2 $(BUILD)/bootloader/stage2.bin --kernel $(BUILD)/vmunix \
		--shell $(BUILD)/bin/sh --nettest $(BUILD)/bin/nettest \
		--bin-file ping=$(BUILD)/bin/ping \
		--bin-file ifconfig=$(BUILD)/bin/ifconfig \
		--bin-file route=$(BUILD)/bin/route \
		--bin-file dhcpcd=$(BUILD)/bin/dhcpcd \
		--bin-file nslookup=$(BUILD)/bin/nslookup $@

$(BUILD)/bios-hdd-image-fragmented.img: $(BUILD)/bootloader/stage1.bin \
	$(BUILD)/bootloader/stage2.bin $(BUILD)/vmunix $(BUILD)/bin/sh \
	$(BUILD)/bin/nettest \
	scripts/make-bios-hdd-image.py scripts/check-bios-hdd-image.py
	$(PYTHON) scripts/make-bios-hdd-image.py --force --machine pcat \
		--stage1 $(BUILD)/bootloader/stage1.bin \
		--stage2 $(BUILD)/bootloader/stage2.bin --kernel $(BUILD)/vmunix \
		--shell $(BUILD)/bin/sh --nettest $(BUILD)/bin/nettest \
		--fragment-kernel $@

$(BUILD)/hdd-image.img: $(BUILD)/bios-hdd-image.img
	cp -f $< $@

bios-hdd-image: $(BUILD)/bios-hdd-image.img
hdd-image: $(BUILD)/hdd-image.img
bios-loader-host-check: $(BUILD)/bios-hdd-image.img
	$(PYTHON) scripts/check-bios-hdd-image.py --machine pcat \
		--kernel $(BUILD)/vmunix $<

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

.PHONY: all vmunix SH bios-bootloader bios-hdd-image hdd-image \
	bios-loader-host-check amd64-hal-compile amd64-entry-qemu-test \
	hdd-boot-qemu-test amd64-qemu-test network-qemu-test

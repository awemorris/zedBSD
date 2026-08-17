# zedBSD NEC PC-9800 architecture rules.
# Copyright (C) 2026 Awe Morris
# SPDX-License-Identifier: Zlib
#
# Included from the top-level Makefile with ARCH=pc98; everything here
# builds into $(BUILD) = build/pc98.

PC98 := platform/pc98
BOOTSECT := bootsectors/pc98
BIOS_LOADER := bootloader/pc98

CIRRUS_NOCT_CFLAGS = $(filter-out -Os,$(NOCT_CFLAGS)) -O2

# These object lists must be defined before the Stage 2 prerequisite list is
# expanded below.  The compiler rules themselves may remain with the related
# verification targets later in this file.
HAL_CC := $(CC) -m32 -march=i386 -ffreestanding -fno-pic -fno-pie \
	-fno-stack-protector -nostdinc -Os -Wall -Wextra -Werror \
	-Iinclude -Iinclude/uapi -Isrc -Isrc/hal/i386 -Ilibc/include \
	-DHAL_ARCH_I386 -DHAL_BOARD_PC98
HAL_PC98_SOURCES := \
	src/hal/cpu-up.c src/hal/i386/lib.c src/hal/i386/irq.c src/hal/i386/page.c \
	src/hal/i386/space.c src/hal/i386/int.c src/hal/i386/cmain.c \
	src/hal/i386/task.c \
	src/hal/i386/bsp-pc98/boot.c \
	src/hal/i386/bsp-pc98/cons.c src/hal/i386/bsp-pc98/pic.c \
	src/hal/i386/bsp-pc98/clock.c src/hal/i386/bsp-pc98/display.c \
	src/hal/i386/bsp-pc98/jisx0208.c
HAL_PC98_ASM := src/hal/i386/locore.S src/hal/i386/trap.S \
	src/hal/i386/dispatch.S
HAL_PC98_OBJS := $(patsubst %.c,$(BUILD)/%.o,$(HAL_PC98_SOURCES)) \
	$(patsubst %.S,$(BUILD)/%.o,$(HAL_PC98_ASM))

ZEDBSD_KERN_CC := $(CC) -m32 -march=i386 -ffreestanding -fno-pic -fno-pie \
	-fno-stack-protector -nostdinc -Os -Wall -Wextra -Werror \
	-Iinclude -Iinclude/uapi -Isrc -I. -Ilibc/include
KERN_OBJS := $(BUILD)/src/kern/entry.o $(BUILD)/src/kern/clock.o \
	$(BUILD)/src/kern/process-timer.o \
	$(BUILD)/src/kern/lock.o $(BUILD)/src/kern/waitq.o \
	$(BUILD)/src/kern/buf.o $(BUILD)/src/kern/sysctl.o \
	$(BUILD)/src/kern/resource.o \
	$(BUILD)/src/kern/resource-limit.o \
	$(BUILD)/src/kern/poll.o \
	$(BUILD)/src/kern/usync.o \
	$(BUILD)/src/kern/process.o $(BUILD)/src/kern/thread.o \
	$(BUILD)/src/kern/sched.o $(BUILD)/src/kern/vmspace.o \
	$(BUILD)/src/kern/vm-object.o $(BUILD)/src/kern/vm-commit.o \
	$(BUILD)/src/kern/filedesc.o $(BUILD)/src/kern/pipe.o \
	$(BUILD)/src/kern/record-lock.o \
	$(BUILD)/src/kern/cred.o $(BUILD)/src/kern/signal.o \
	$(KERN_ACL_OBJS) \
	$(KERN_QUOTA_OBJS) \
	$(BUILD)/src/kern/cwdinfo.o \
	$(BUILD)/src/kern/elf.o $(BUILD)/src/kern/exec.o \
	$(BUILD)/src/kern/user-probe.o $(BUILD)/src/kern/syscall.o \
	$(BUILD)/src/kern/uaccess.o $(BUILD)/src/kern/cdev.o \
	$(BUILD)/src/kern/devfs.o $(BUILD)/src/kern/console-device.o \
	$(BUILD)/src/kern/tty.o \
	$(BUILD)/src/kern/graphics-device.o $(BUILD)/src/kern/pc98/font.o \
	$(BUILD)/src/kern/system-device.o \
	$(BUILD)/src/kern/init.o \
	$(BUILD)/drivers/pc98-graphics.o \
	$(KERN_NET_OBJS) \
	$(KERN_UFS1_OBJS) $(KERN_UFS2_OBJS) $(KERN_UFS_CONSISTENCY_OBJS)

# Milestone verification nests QEMU tests.  Keep those chains ordered even
# when the caller requests a highly parallel compile.
.NOTPARALLEL: noct-m9-verify noct-m10-verify noct-m11-verify \
	noct-m14-verify noct-m15-verify noct-m17-verify beui-g2b-verify \
	beui-g2c-verify beui-g5-verify

# BeUI display backends this target selects from upstream Noct.  The
# Core-Graph blitter is the one hot loop in the graphical path, so it
# trades size for speed while the rest of the image stays at -Os.
PC98_BEUI_OBJS := \
	$(NOCT_BUILD_DIR)/beui-pc98-gdc.o \
	$(NOCT_BUILD_DIR)/beui-pc98-glyph.o \
	$(NOCT_BUILD_DIR)/beui-pc98-cirrus.o \
	$(NOCT_BUILD_DIR)/beui-pc98-auto.o

STAGE2_OBJS = \
	$(BUILD)/$(PC98)/boot-header.o \
	$(BUILD)/src/kern/main.o \
	$(BUILD)/src/kern/env.o \
	$(BUILD)/src/kern/fs.o \
	$(BUILD)/src/kern/namespace.o \
	$(PC98_BEUI_OBJS) \
	$(BUILD)/src/kern/fat.o \
	$(BUILD)/src/kern/fat-lfn.o \
	$(BUILD)/src/kern/fat16.o \
	$(BUILD)/src/kern/fat-vfs.o \
	$(BUILD)/src/kern/inode.o \
	$(BUILD)/src/kern/file.o \
	$(BUILD)/src/kern/namecache.o \
	$(BUILD)/src/kern/namei.o \
	$(BUILD)/src/kern/mount.o \
	$(BUILD)/src/kern/rootfs.o \
	$(BUILD)/src/kern/tmpfs.o \
	$(BUILD)/src/kern/overlayfs.o \
	$(BUILD)/src/kern/vfs.o \
	$(BUILD)/src/kern/swap.o \
	$(BUILD)/src/kern/swap-fat.o \
	$(BUILD)/src/kern/vm-reclaim.o \
	$(BUILD)/src/kern/disk.o \
	$(BUILD)/src/kern/partition.o \
	$(BUILD)/drivers/loop.o \
	$(BUILD)/drivers/pc98-ide.o \
	$(BUILD)/drivers/dp8390.o \
	$(BUILD)/drivers/pc98-lgy98.o \
	$(BUILD)/src/kern/mbr-partition.o \
	$(BUILD)/src/kern/pc98/partition.o \
	$(BUILD)/src/kern/pc98/partition-auto.o \
	$(BUILD)/src/kern/pc98/platform.o \
	$(BUILD)/src/kern/image.o \
	$(BUILD)/src/kern/panic.o \
	$(ZEDBSD_LIBC_OBJECTS) \
	$(HAL_PC98_OBJS) $(KERN_OBJS)
M9_STAGE2_OBJS = $(filter-out $(BUILD)/src/kern/main.o \
	$(BUILD)/src/kern/shell.o $(BUILD)/src/kern/device.o,$(STAGE2_OBJS)) \
	$(BUILD)/$(PC98)/stage2-m9-test.o \
	$(BUILD)/$(PC98)/shell-m9-test.o \
	$(BUILD)/$(PC98)/device-m9-test.o

all: $(BUILD)/ipl-lba0.bin $(BUILD)/ipl-lba2.bin \
	$(BUILD)/ipl-lba0.img $(BUILD)/ipl-lba2.img $(BUILD)/ipl-part.img \
	$(BUILD)/IO.SYS $(BUILD)/vmunix \
	$(BUILD)/INIT.ELF $(BUILD)/bin/noct $(BUILD)/bin/sh \
	$(BUILD)/bin/nettest $(BUILD)/bin/ping \
	$(BUILD)/bin/ifconfig $(BUILD)/bin/route $(BUILD)/bin/dhcpcd \
	$(BUILD)/bin/nslookup $(BUILD)/bin/sysctl $(BUILD)/bin/mount \
	$(BUILD)/bin/umount \
	$(BUILD)/partition-pbr.bin \
	$(BUILD)/chain-test.bin $(BUILD)/fdd-ipl.bin \
	$(BUILD)/BOOTAPP.BIN

# Convenience aliases for the primary artifacts.
vmunix: $(BUILD)/vmunix
vmunix-m9: $(BUILD)/vmunix-m9
BOOTAPP.BIN: $(BUILD)/BOOTAPP.BIN
INIT.ELF: $(BUILD)/INIT.ELF
NOCT.ELF: $(BUILD)/NOCT.ELF
SH: $(BUILD)/bin/sh
USER-FAULT.ELF: $(BUILD)/USER-FAULT.ELF
USER-SWAP.ELF: $(BUILD)/USER-SWAP.ELF
USER-STACK.ELF: $(BUILD)/USER-STACK.ELF
USER-STACK-GUARD.ELF: $(BUILD)/USER-STACK-GUARD.ELF
POSIX-R2.ELF: $(BUILD)/POSIX-R2.ELF
POSIX-R2-REMAINING.ELF: $(BUILD)/POSIX-R2-REMAINING.ELF
.PHONY: vmunix vmunix-m9 BOOTAPP.BIN INIT.ELF NOCT.ELF SH \
	USER-FAULT.ELF USER-SWAP.ELF USER-STACK.ELF USER-STACK-GUARD.ELF \
	POSIX-R2.ELF POSIX-R2-REMAINING.ELF

# ----------------------------------------------------------------------
# Per-object flag overrides.

$(NOCT_BUILD_DIR)/beui-pc98-cirrus.o: NOCT_CFLAGS := $(CIRRUS_NOCT_CFLAGS)

NOCT_GLUE_OBJS := $(BUILD)/userland/noct/integration/noct.o $(BUILD)/userland/noct/integration/napi.o \
	$(BUILD)/userland/noct/integration/target.o
$(NOCT_GLUE_OBJS): OBJ_CPPFLAGS = $(NOCT_CPPFLAGS) -Iinclude -Isrc
$(NOCT_GLUE_OBJS): OBJ_CFLAGS = $(NOCT_CFLAGS)
$(BUILD)/drivers/pc98-graphics.o: OBJ_CPPFLAGS = $(NOCT_CPPFLAGS) -Iinclude -Isrc
$(BUILD)/drivers/pc98-graphics.o: OBJ_CFLAGS = $(ZEDBSD_CFLAGS)
$(BUILD)/userland/noct/integration/platform.o: OBJ_CPPFLAGS = $(NOCT_CPPFLAGS) \
	$(ZEDBSD_LIBC_CPPFLAGS)
$(BUILD)/userland/noct/integration/platform.o: OBJ_CFLAGS = $(ZEDBSD_LIBC_CFLAGS)

STAGE2_CPPFLAGS = $(ZEDBSD_CPPFLAGS)

$(BUILD)/src/kern/startup.o $(BUILD)/$(PC98)/stage2-m9-test.o: \
	$(BUILD)/kern/messages.h

$(BUILD)/$(PC98)/stage2-m9-test.o: src/kern/main.c
	@mkdir -p $(dir $@)
	$(CC) $(STAGE2_CPPFLAGS) $(ZEDBSD_CFLAGS) -DZEDBSD_M9_WRITE_TEST \
		-MMD -MP -c $< -o $@

$(BUILD)/$(PC98)/shell-m9-test.o: src/kern/shell.c
	@mkdir -p $(dir $@)
	$(CC) $(STAGE2_CPPFLAGS) $(ZEDBSD_CFLAGS) -DZEDBSD_M9_WRITE_TEST \
		-MMD -MP -c $< -o $@

$(BUILD)/$(PC98)/device-m9-test.o: src/kern/device.c
	@mkdir -p $(dir $@)
	$(CC) $(ZEDBSD_CPPFLAGS) $(ZEDBSD_CFLAGS) -DZEDBSD_M9_WRITE_TEST \
		-MMD -MP -c $< -o $@

# ----------------------------------------------------------------------
# Flat binaries from the 16-bit boot-sector world.
# link-flat: name, object, text address.

define link-flat
$(BUILD)/$(1).elf: $(BUILD)/$(BOOTSECT)/$(2).o
	$$(LD) -m elf_i386 -Ttext=$(3) -e _start $$< -o $$@
endef

$(eval $(call link-flat,disk-ipl,disk-ipl,0))
$(eval $(call link-flat,lba2,lba2,0))
$(eval $(call link-flat,partition-pbr,partition-pbr,0))
$(eval $(call link-flat,stage1,stage1,0))
$(eval $(call link-flat,chain-test,chain-test,0))
$(eval $(call link-flat,fdd-ipl,fdd-ipl,0))

$(BUILD)/ipl-lba0.bin: $(BUILD)/disk-ipl.elf
	$(OBJCOPY) -O binary -j .text $< $@
	@test $$(stat -c%s $@) -eq 512
	@test "$$(od -An -tx1 -j508 -N4 $@ | tr -d ' \n')" = 090055aa

# Native PC-98 BIOS code using a PC/AT-compatible MBR disk layout.
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
	@test "$$(od -An -tx1 -j510 -N2 $@ | tr -d ' \n')" = 55aa

$(BUILD)/bootloader/stage2.o: $(BIOS_LOADER)/stage2.S \
	bootloader/include/disk-layout.inc bootloader/include/stage2-header.inc \
	bootloader/include/mbr.inc bootloader/include/fat16.inc \
	bootloader/include/elf.inc
	@mkdir -p $(dir $@)
	$(CC) -m64 -I. -x assembler-with-cpp -c $< -o $@

$(BUILD)/bootloader/stage2.elf: $(BUILD)/bootloader/stage2.o \
	$(BIOS_LOADER)/stage2.ld
	$(LD) -m elf_x86_64 -T $(BIOS_LOADER)/stage2.ld $< -o $@

$(BUILD)/bootloader/stage2.raw: $(BUILD)/bootloader/stage2.elf
	$(OBJCOPY) -O binary -j .text $< $@

$(BUILD)/bootloader/stage2.bin: $(BUILD)/bootloader/stage2.raw \
	$(SCRIPTS_DIR)/finalize-bios-stage2.py
	$(PYTHON) $(SCRIPTS_DIR)/finalize-bios-stage2.py --machine pc98 $< $@

$(BUILD)/bootloader/payload32.o: bootloader/tests/payload32-pc98.S
	@mkdir -p $(dir $@)
	$(CC) -m32 -c $< -o $@

$(BUILD)/bootloader/payload32.elf: $(BUILD)/bootloader/payload32.o \
	bootloader/tests/payload32-pc98.ld
	$(LD) -m elf_i386 -T bootloader/tests/payload32-pc98.ld $< -o $@

bios-bootloader: $(BUILD)/bootloader/stage1.bin \
	$(BUILD)/bootloader/stage2.bin

USER_BASIC_COMMANDS := basename dirname cat mkdir rmdir cp mv rm unlink ln link touch readlink truncate chmod chown chgrp mkfifo stat uname df tty sleep head tail wc tee cmp cksum strings id kill
USER_BASIC_TARGETS := $(addprefix $(BUILD)/bin/,$(USER_BASIC_COMMANDS))

I386_ARCH_IMAGE := $(ARCH_IMAGE_DIR)/i386.img
I386_ARCH_INPUTS := $(BUILD)/bin/sh $(BUILD)/bin/noct \
	$(BUILD)/bin/nettest $(BUILD)/bin/ping $(BUILD)/bin/ifconfig \
	$(BUILD)/bin/route $(BUILD)/bin/dhcpcd $(BUILD)/bin/nslookup \
	$(BUILD)/bin/sysctl $(BUILD)/bin/mount $(BUILD)/bin/umount \
	$(BUILD)/dynamic/ld.so $(BUILD)/dynamic/libc.so \
	$(BUILD)/dynamic/tlstest.so $(BUILD)/dynamic/dyntest \
	$(BUILD)/dynamic/alt/rpathdep.so $(BUILD)/dynamic/rpathtest.so \
	$(BUILD)/dynamic/verstest.so $(BUILD)/dynamic/versuse.so
I386_ARCH_FILES := --file /bin/sh=$(BUILD)/bin/sh \
	--file /bin/noct=$(BUILD)/bin/noct \
	--file /bin/nettest=$(BUILD)/bin/nettest \
	--file /bin/ping=$(BUILD)/bin/ping \
	--file /bin/ifconfig=$(BUILD)/bin/ifconfig \
	--file /bin/route=$(BUILD)/bin/route \
	--file /bin/dhcpcd=$(BUILD)/bin/dhcpcd \
	--file /bin/nslookup=$(BUILD)/bin/nslookup \
	--file /bin/sysctl=$(BUILD)/bin/sysctl \
	--file /bin/mount=$(BUILD)/bin/mount \
	--file /bin/umount=$(BUILD)/bin/umount \
	--file /lib/ld.so=$(BUILD)/dynamic/ld.so \
	--file /lib/libc.so=$(BUILD)/dynamic/libc.so \
	--file /lib/tlstest.so=$(BUILD)/dynamic/tlstest.so \
	--file /lib/alt/rpathdep.so=$(BUILD)/dynamic/alt/rpathdep.so \
	--file /lib/rpthtest.so=$(BUILD)/dynamic/rpathtest.so \
	--file /lib/verstest.so=$(BUILD)/dynamic/verstest.so \
	--file /lib/versuse.so=$(BUILD)/dynamic/versuse.so \
	--file /bin/dyntest=$(BUILD)/dynamic/dyntest
I386_ARCH_INPUTS += $(USER_BASIC_TARGETS)
I386_ARCH_FILES += $(foreach command,$(USER_BASIC_COMMANDS),--file /bin/$(command)=$(BUILD)/bin/$(command))
$(eval $(call ZEDBSD_ARCH_IMAGE_RULE,$(I386_ARCH_IMAGE),i386,$(I386_ARCH_INPUTS),$(I386_ARCH_FILES)))
I386_ARCH_UFS_IMAGE := $(ARCH_IMAGE_DIR)/i386.ufs
$(eval $(call ZEDBSD_ARCH_UFS_IMAGE_RULE,$(I386_ARCH_UFS_IMAGE),i386,$(I386_ARCH_INPUTS),$(I386_ARCH_FILES)))
arch-image: $(I386_ARCH_IMAGE)
arch-image-check: $(I386_ARCH_IMAGE)-check
arch-image-ufs: $(I386_ARCH_UFS_IMAGE)
arch-image-ufs-check: $(I386_ARCH_UFS_IMAGE)-check

$(BUILD)/bios-hdd-image.img: $(BUILD)/bootloader/stage1.bin \
	$(BUILD)/bootloader/stage2.bin $(BUILD)/vmunix $(I386_ARCH_IMAGE) \
	$(HOLORIS_NOCT) \
	$(SCRIPTS_DIR)/make-bios-hdd-image.py \
	$(SCRIPTS_DIR)/check-bios-hdd-image.py
	$(PYTHON) $(SCRIPTS_DIR)/make-bios-hdd-image.py --force \
		--machine pc98 --stage1 $(BUILD)/bootloader/stage1.bin \
		--stage2 $(BUILD)/bootloader/stage2.bin --kernel $(BUILD)/vmunix \
		--arch-profile i386 --arch-image $(I386_ARCH_IMAGE) \
		--holoris $(HOLORIS_NOCT) $@

$(BUILD)/ufs-root.img: $(I386_ARCH_UFS_IMAGE) \
	$(SCRIPTS_DIR)/make-ufs1-root-image.py scripts/ufs1_format.py
	$(PYTHON) $(SCRIPTS_DIR)/make-ufs1-root-image.py --force \
		--arch-profile i386 --arch-image $(I386_ARCH_UFS_IMAGE) $@

$(BUILD)/ufs-root-hdd-image.img: $(BUILD)/bootloader/stage1.bin \
	$(BUILD)/bootloader/stage2.bin $(BUILD)/vmunix $(BUILD)/ufs-root.img \
	$(SCRIPTS_DIR)/make-bios-hdd-image.py
	$(PYTHON) $(SCRIPTS_DIR)/make-bios-hdd-image.py --force \
		--machine pc98 --stage1 $(BUILD)/bootloader/stage1.bin \
		--stage2 $(BUILD)/bootloader/stage2.bin --kernel $(BUILD)/vmunix \
		--ufs-root $(BUILD)/ufs-root.img --size-mib 193 $@

ufs-root-image: $(BUILD)/ufs-root-hdd-image.img

bios-hdd-image: $(BUILD)/bios-hdd-image.img
bios-loader-host-check: $(BUILD)/bios-hdd-image.img
	$(PYTHON) $(SCRIPTS_DIR)/check-bios-hdd-image.py --machine pc98 \
		--kernel $(BUILD)/vmunix --arch-profile i386 \
		--arch-image $(I386_ARCH_IMAGE) \
		--holoris $(HOLORIS_NOCT) $<

bios-loader-qemu-test: bios-bootloader $(BUILD)/bootloader/payload32.elf
	bash $(SCRIPTS_DIR)/test-bios-bootloader-qemu.sh pc98

network-qemu-test: $(BUILD)/bootloader/stage1.bin \
	$(BUILD)/bootloader/stage2.bin $(BUILD)/vmunix \
	$(BUILD)/bin/nettest
	bash $(SCRIPTS_DIR)/test-pc98-network.sh

$(BUILD)/hdd-image.img: $(BUILD)/bios-hdd-image.img
	cp -f $< $@

$(BUILD)/legacy-nec98-hdd-image.img: $(BUILD)/hdd-test.img
	cp -f $< $@

legacy-pc98-hdd-image: $(BUILD)/legacy-nec98-hdd-image.img

.PHONY: arch-image arch-image-check arch-image-ufs arch-image-ufs-check \
	ufs-root-image bios-bootloader bios-hdd-image bios-loader-host-check \
	bios-loader-qemu-test \
	legacy-pc98-hdd-image

$(BUILD)/ipl-lba2.bin: $(BUILD)/lba2.elf
	$(OBJCOPY) -O binary -j .text $< $@
	@test $$(stat -c%s $@) -eq 7168

$(BUILD)/partition-pbr.bin: $(BUILD)/partition-pbr.elf
	$(OBJCOPY) -O binary -j .text $< $@
	@test $$(stat -c%s $@) -eq 1024


$(BUILD)/IO.SYS: $(BUILD)/stage1.elf
	$(OBJCOPY) -O binary -j .text $< $@
	@sz=$$(stat -c%s $@); echo "IO.SYS: $$sz bytes"; test $$sz -le 65024

$(BUILD)/chain-test.bin: $(BUILD)/chain-test.elf
	$(OBJCOPY) -O binary -j .text $< $@
	@test $$(stat -c%s $@) -eq 512

$(BUILD)/fdd-ipl.bin: $(BUILD)/fdd-ipl.elf
	$(OBJCOPY) -O binary -j .text $< $@
	@test $$(stat -c%s $@) -eq 512

# Staging copies used by disk-image tooling.
$(BUILD)/ipl-lba0.img: $(BUILD)/ipl-lba0.bin
	cp $< $@
$(BUILD)/ipl-lba2.img: $(BUILD)/ipl-lba2.bin
	cp $< $@
$(BUILD)/ipl-part.img: $(BUILD)/partition-pbr.bin
	cp $< $@

# ----------------------------------------------------------------------
# Stage 2 (vmunix) and the applet container.

USER_LIBC_OBJS := $(BUILD)/userland/crt0.o $(BUILD)/userland/libc/posix.o \
	$(BUILD)/userland/libc/dlfcn.o \
	$(BUILD)/userland/libc/static-tls.o \
	$(BUILD)/userland/libc/poll.o $(BUILD)/userland/libc/termios.o \
	$(BUILD)/userland/libc/pthread.o \
	$(BUILD)/userland/libc/shm.o \
	$(BUILD)/userland/libc/semaphore.o \
	$(BUILD)/userland/libc/mqueue.o \
	$(BUILD)/userland/libc/socket.o $(BUILD)/userland/libc/resolver.o \
	$(BUILD)/userland/libc/resolver-dns.o $(BUILD)/userland/libc/signal.o \
	$(BUILD)/libc/heap.o $(BUILD)/libc/string.o $(BUILD)/libc/ctype.o \
	$(BUILD)/libc/locale.o $(BUILD)/libc/wide.o \
	$(BUILD)/libc/int64.o $(BUILD)/libc/strto.o $(BUILD)/libc/format.o \
	$(BUILD)/libc/stdio.o
USER_CFLAGS := $(ZEDBSD_CFLAGS) -fno-builtin -ffunction-sections \
	-fdata-sections -msoft-float -mno-80387 -mno-fp-ret-in-387 \
	-mno-mmx -mno-sse -mno-sse2
USER_STACK_LDFLAGS := -z stack-size=0x100000
USER_ELF_CHECK := $(SCRIPTS_DIR)/check-user-elf.py
$(BUILD)/userland/libc/posix.o $(BUILD)/userland/libc/poll.o \
	$(BUILD)/userland/libc/termios.o \
	$(BUILD)/userland/libc/pthread.o \
	$(BUILD)/userland/libc/socket.o \
	$(BUILD)/userland/tests/syscall-smoke.o \
	$(BUILD)/userland/tests/posix-r2.o \
	$(BUILD)/userland/tests/posix-r2-remaining.o: \
	OBJ_CPPFLAGS = $(ZEDBSD_CPPFLAGS)
$(BUILD)/userland/libc/posix.o $(BUILD)/userland/libc/poll.o \
	$(BUILD)/userland/libc/termios.o \
	$(BUILD)/userland/libc/pthread.o \
	$(BUILD)/userland/libc/socket.o \
	$(BUILD)/userland/tests/syscall-smoke.o \
	$(BUILD)/userland/tests/posix-r2.o \
	$(BUILD)/userland/tests/posix-r2-remaining.o: \
	OBJ_CFLAGS = $(USER_CFLAGS)

$(BUILD)/INIT.ELF: $(USER_LIBC_OBJS) $(BUILD)/userland/tests/syscall-smoke.o \
	$(ZEDBSD_SOFTFLOAT_OBJECTS) $(PC98)/noct-user.ld $(USER_ELF_CHECK)
	$(LD) -m elf_i386 --gc-sections -nostdlib -static -z max-page-size=4096 \
		$(USER_STACK_LDFLAGS) \
		-T $(PC98)/noct-user.ld $(USER_LIBC_OBJS) \
		$(BUILD)/userland/tests/syscall-smoke.o \
		$(ZEDBSD_SOFTFLOAT_OBJECTS) -o $@
	$(PYTHON) $(USER_ELF_CHECK) $@

USER_NOCT_GLUE_OBJS := $(BUILD)/userland/noct/runtime/main.o \
	$(BUILD)/userland/noct/runtime/memory.o \
	$(BUILD)/userland/noct/runtime/platform.o \
	$(BUILD)/userland/noct/runtime/env.o \
	$(BUILD)/userland/noct/integration/napi.o \
	$(BUILD)/userland/noct/integration/target.o
$(USER_NOCT_GLUE_OBJS): OBJ_CPPFLAGS = $(USER_NOCT_CPPFLAGS) -Iinclude -Isrc
$(USER_NOCT_GLUE_OBJS): OBJ_CFLAGS = $(USER_CFLAGS)
$(BUILD)/userland/noct/integration/napi.o: userland/noct/integration/napi.c
	@mkdir -p $(dir $@)
	$(CC) $(USER_NOCT_CPPFLAGS) -Iinclude -Isrc $(USER_CFLAGS) -MMD -MP -c $< -o $@
$(BUILD)/userland/noct/integration/target.o: userland/noct/integration/target.c
	@mkdir -p $(dir $@)
	$(CC) $(USER_NOCT_CPPFLAGS) -Iinclude -Isrc $(USER_CFLAGS) -MMD -MP -c $< -o $@

$(BUILD)/NOCT.ELF: $(USER_LIBC_OBJS) $(USER_NOCT_GLUE_OBJS) \
	$(USER_NOCT_OBJECTS) $(ZEDBSD_SOFTFLOAT_OBJECTS) $(PC98)/noct-user.ld \
	$(USER_ELF_CHECK)
	$(LD) -m elf_i386 --gc-sections -nostdlib -static -z max-page-size=4096 \
		$(USER_STACK_LDFLAGS) \
		-T $(PC98)/noct-user.ld $(USER_LIBC_OBJS) $(USER_NOCT_GLUE_OBJS) \
		$(USER_NOCT_OBJECTS) $(ZEDBSD_SOFTFLOAT_OBJECTS) -o $@
	@test -z "$$($(NOCT_NM) -u $@)" || { $(NOCT_NM) -u $@; exit 1; }
	$(PYTHON) $(USER_ELF_CHECK) $@

$(BUILD)/bin/noct: $(BUILD)/NOCT.ELF
	@mkdir -p $(dir $@)
	cp $< $@
	$(PYTHON) $(USER_ELF_CHECK) $@

USER_SH_OBJS := $(BUILD)/userland/sh/main.o $(BUILD)/userland/sh/applet.o \
	$(BUILD)/userland/sh/builtins.o $(BUILD)/userland/sh/lexer.o \
	$(BUILD)/userland/sh/expand.o
$(USER_SH_OBJS): OBJ_CPPFLAGS = $(ZEDBSD_CPPFLAGS)
$(USER_SH_OBJS): OBJ_CFLAGS = $(USER_CFLAGS)

$(BUILD)/bin/sh: $(USER_LIBC_OBJS) $(USER_SH_OBJS) \
	$(ZEDBSD_SOFTFLOAT_OBJECTS) $(PC98)/noct-user.ld $(USER_ELF_CHECK)
	@mkdir -p $(dir $@)
	$(LD) -m elf_i386 --gc-sections -nostdlib -static -z max-page-size=4096 \
		$(USER_STACK_LDFLAGS) \
		-T $(PC98)/noct-user.ld $(USER_LIBC_OBJS) $(USER_SH_OBJS) \
		$(ZEDBSD_SOFTFLOAT_OBJECTS) -o $@
	@test -z "$$($(NOCT_NM) -u $@)" || { $(NOCT_NM) -u $@; exit 1; }
	$(PYTHON) $(USER_ELF_CHECK) $@

$(BUILD)/POSIX-R2.ELF: $(USER_LIBC_OBJS) \
	$(BUILD)/userland/tests/posix-r2.o $(PC98)/noct-user.ld \
	$(ZEDBSD_SOFTFLOAT_OBJECTS) $(USER_ELF_CHECK)
	$(LD) -m elf_i386 --gc-sections -nostdlib -static -z max-page-size=4096 \
		$(USER_STACK_LDFLAGS) -T $(PC98)/noct-user.ld \
		$(USER_LIBC_OBJS) $(BUILD)/userland/tests/posix-r2.o \
		$(ZEDBSD_SOFTFLOAT_OBJECTS) -o $@
	$(PYTHON) $(USER_ELF_CHECK) $@

$(BUILD)/POSIX-R2-REMAINING.ELF: $(USER_LIBC_OBJS) \
	$(BUILD)/userland/tests/posix-r2-remaining.o $(PC98)/noct-user.ld \
	$(ZEDBSD_SOFTFLOAT_OBJECTS) $(USER_ELF_CHECK)
	$(LD) -m elf_i386 --gc-sections -nostdlib -static -z max-page-size=4096 \
		$(USER_STACK_LDFLAGS) -T $(PC98)/noct-user.ld \
		$(USER_LIBC_OBJS) $(BUILD)/userland/tests/posix-r2-remaining.o \
		$(ZEDBSD_SOFTFLOAT_OBJECTS) -o $@
	$(PYTHON) $(USER_ELF_CHECK) $@

USER_SYSCTL_OBJ := $(BUILD)/userland/sysctl/main.o
$(USER_SYSCTL_OBJ): OBJ_CPPFLAGS = $(ZEDBSD_CPPFLAGS)
$(USER_SYSCTL_OBJ): OBJ_CFLAGS = $(USER_CFLAGS)
$(BUILD)/bin/sysctl: $(USER_LIBC_OBJS) $(USER_SYSCTL_OBJ) \
	$(ZEDBSD_SOFTFLOAT_OBJECTS) $(PC98)/noct-user.ld $(USER_ELF_CHECK)
	@mkdir -p $(dir $@)
	$(LD) -m elf_i386 --gc-sections -nostdlib -static -z max-page-size=4096 \
		$(USER_STACK_LDFLAGS) -T $(PC98)/noct-user.ld \
		$(USER_LIBC_OBJS) $(USER_SYSCTL_OBJ) \
		$(ZEDBSD_SOFTFLOAT_OBJECTS) -o $@
	@test -z "$$($(NOCT_NM) -u $@)" || { $(NOCT_NM) -u $@; exit 1; }
	$(PYTHON) $(USER_ELF_CHECK) $@

USER_MOUNT_OBJ := $(BUILD)/userland/mount/main.o
$(USER_MOUNT_OBJ): OBJ_CPPFLAGS = $(ZEDBSD_CPPFLAGS)
$(USER_MOUNT_OBJ): OBJ_CFLAGS = $(USER_CFLAGS)
$(BUILD)/bin/mount: $(USER_LIBC_OBJS) $(USER_MOUNT_OBJ) \
	$(ZEDBSD_SOFTFLOAT_OBJECTS) $(PC98)/noct-user.ld $(USER_ELF_CHECK)
	@mkdir -p $(dir $@)
	$(LD) -m elf_i386 --gc-sections -nostdlib -static -z max-page-size=4096 \
		$(USER_STACK_LDFLAGS) -T $(PC98)/noct-user.ld \
		$(USER_LIBC_OBJS) $(USER_MOUNT_OBJ) \
		$(ZEDBSD_SOFTFLOAT_OBJECTS) -o $@
	@test -z "$$($(NOCT_NM) -u $@)" || { $(NOCT_NM) -u $@; exit 1; }
	$(PYTHON) $(USER_ELF_CHECK) $@
$(BUILD)/bin/umount: $(BUILD)/bin/mount
	@mkdir -p $(dir $@)
	cp -f $< $@

USER_NETTEST_OBJS := $(BUILD)/userland/nettest/main.o
$(USER_NETTEST_OBJS): OBJ_CPPFLAGS = $(ZEDBSD_CPPFLAGS)
$(USER_NETTEST_OBJS): OBJ_CFLAGS = $(USER_CFLAGS)

$(BUILD)/bin/nettest: $(USER_LIBC_OBJS) $(USER_NETTEST_OBJS) \
	$(ZEDBSD_SOFTFLOAT_OBJECTS) $(PC98)/noct-user.ld $(USER_ELF_CHECK)
	@mkdir -p $(dir $@)
	$(LD) -m elf_i386 --gc-sections -nostdlib -static -z max-page-size=4096 \
		$(USER_STACK_LDFLAGS) -T $(PC98)/noct-user.ld \
		$(USER_LIBC_OBJS) $(USER_NETTEST_OBJS) \
		$(ZEDBSD_SOFTFLOAT_OBJECTS) -o $@
	@test -z "$$($(NOCT_NM) -u $@)" || { $(NOCT_NM) -u $@; exit 1; }
	$(PYTHON) $(USER_ELF_CHECK) $@

USER_NET_COMMANDS := ping ifconfig route dhcpcd nslookup
USER_NET_COMMAND_TARGETS := $(addprefix $(BUILD)/bin/,$(USER_NET_COMMANDS))
USER_NET_COMMON_OBJS := $(BUILD)/userland/net/netutil.o \
	$(BUILD)/userland/net/dhcp.o
USER_NET_COMMAND_OBJS := $(addsuffix /main.o, \
	$(addprefix $(BUILD)/userland/,$(USER_NET_COMMANDS)))
$(BUILD)/userland/libc/resolver.o $(BUILD)/userland/libc/resolver-dns.o \
	$(USER_NET_COMMON_OBJS) $(USER_NET_COMMAND_OBJS): \
	OBJ_CPPFLAGS = $(ZEDBSD_CPPFLAGS)
$(BUILD)/userland/libc/resolver.o $(BUILD)/userland/libc/resolver-dns.o \
	$(USER_NET_COMMON_OBJS) $(USER_NET_COMMAND_OBJS): \
	OBJ_CFLAGS = $(USER_CFLAGS)

define PC98_USER_NET_COMMAND
$(BUILD)/bin/$(1): $(USER_LIBC_OBJS) $(USER_NET_COMMON_OBJS) \
	$(BUILD)/userland/$(1)/main.o $(ZEDBSD_SOFTFLOAT_OBJECTS) \
	$(PC98)/noct-user.ld $(USER_ELF_CHECK)
	@mkdir -p $$(dir $$@)
	$(LD) -m elf_i386 --gc-sections -nostdlib -static -z max-page-size=4096 \
		$(USER_STACK_LDFLAGS) -T $(PC98)/noct-user.ld \
		$(USER_LIBC_OBJS) $(USER_NET_COMMON_OBJS) \
		$(BUILD)/userland/$(1)/main.o $(ZEDBSD_SOFTFLOAT_OBJECTS) -o $$@
	@test -z "$$$$($(NOCT_NM) -u $$@)" || { $(NOCT_NM) -u $$@; exit 1; }
	$(PYTHON) $(USER_ELF_CHECK) $$@
endef
$(foreach command,$(USER_NET_COMMANDS),\
	$(eval $(call PC98_USER_NET_COMMAND,$(command))))
network-tools: $(USER_NET_COMMAND_TARGETS)
.PHONY: network-tools

USER_BASIC_COMMON_OBJ := $(BUILD)/userland/common/command.o
USER_BASIC_COMMAND_OBJS := $(addsuffix /main.o, \
	$(addprefix $(BUILD)/userland/,$(USER_BASIC_COMMANDS)))
$(USER_BASIC_COMMON_OBJ) $(USER_BASIC_COMMAND_OBJS): OBJ_CPPFLAGS = $(ZEDBSD_CPPFLAGS)
$(USER_BASIC_COMMON_OBJ) $(USER_BASIC_COMMAND_OBJS): OBJ_CFLAGS = $(USER_CFLAGS)

define PC98_USER_BASIC_COMMAND
$(BUILD)/bin/$(1): $(USER_LIBC_OBJS) $(USER_BASIC_COMMON_OBJ) \
	$(BUILD)/userland/$(1)/main.o $(ZEDBSD_SOFTFLOAT_OBJECTS) \
	$(PC98)/noct-user.ld $(USER_ELF_CHECK)
	@mkdir -p $$(dir $$@)
	$(LD) -m elf_i386 --gc-sections -nostdlib -static -z max-page-size=4096 \
		$(USER_STACK_LDFLAGS) -T $(PC98)/noct-user.ld \
		$(USER_LIBC_OBJS) $(USER_BASIC_COMMON_OBJ) \
		$(BUILD)/userland/$(1)/main.o $(ZEDBSD_SOFTFLOAT_OBJECTS) -o $$@
	@test -z "$$$$($(NOCT_NM) -u $$@)" || { $(NOCT_NM) -u $$@; exit 1; }
	$(PYTHON) $(USER_ELF_CHECK) $$@
endef
$(foreach command,$(USER_BASIC_COMMANDS),\
	$(eval $(call PC98_USER_BASIC_COMMAND,$(command))))
basic-tools: $(USER_BASIC_TARGETS)
.PHONY: basic-tools

# ELF32 runtime linker and shared libc.  PC-98 and PC/AT intentionally use
# the same i386 user ABI; only their HAL and boot paths differ.
DYNAMIC_DIR := $(BUILD)/dynamic
DYNAMIC_CPPFLAGS := -nostdinc -I. -Iinclude -Iinclude/uapi -Ilibc/include \
	-DHAL_ARCH_I386 -DZEDBSD_DYNAMIC_LIBC
DYNAMIC_CFLAGS := -m32 -march=i386 -Os -ffreestanding -fPIC -fno-builtin \
	-fno-stack-protector -fno-asynchronous-unwind-tables -fno-unwind-tables \
	-ftls-model=global-dynamic -Wall -Wextra -Werror -msoft-float \
	-mno-80387 -mno-fp-ret-in-387 -mno-mmx -mno-sse -mno-sse2
DYNAMIC_LIBC_SOURCES := userland/libc/posix.c userland/libc/poll.c \
	userland/libc/termios.c userland/libc/pthread.c userland/libc/shm.c \
	userland/libc/semaphore.c userland/libc/mqueue.c userland/libc/dlfcn.c \
	userland/libc/socket.c userland/libc/resolver.c \
	userland/libc/resolver-dns.c userland/libc/signal.c libc/heap.c \
	libc/string.c libc/ctype.c libc/locale.c libc/wide.c libc/int64.c \
	libc/strto.c libc/format.c \
	libc/stdio.c
DYNAMIC_LIBC_OBJS := $(patsubst %.c,$(DYNAMIC_DIR)/obj/%.o,\
	$(DYNAMIC_LIBC_SOURCES)) $(DYNAMIC_DIR)/obj/userland/libc/syscall.o
DYNAMIC_RTLD_OBJS := $(DYNAMIC_DIR)/obj/userland/rtld/entry.o \
	$(DYNAMIC_DIR)/obj/userland/rtld/rtld.o \
	$(DYNAMIC_DIR)/obj/userland/rtld/string.o
DYNAMIC_SOFTFLOAT_DIR := $(DYNAMIC_DIR)/softfloat
DYNAMIC_GCC_SOFTFLOAT_OBJS := $(addprefix $(DYNAMIC_SOFTFLOAT_DIR)/gcc-,\
	$(ZEDBSD_GCC_SOFTFP_REL:.c=.o))
DYNAMIC_MUSL_MATH_OBJS := $(addprefix $(DYNAMIC_SOFTFLOAT_DIR)/musl-,\
	$(ZEDBSD_MUSL_MATH_REL:.c=.o))
DYNAMIC_MUSL_SCAN_OBJS := $(DYNAMIC_SOFTFLOAT_DIR)/musl-shgetc.o \
	$(DYNAMIC_SOFTFLOAT_DIR)/musl-floatscan.o \
	$(DYNAMIC_SOFTFLOAT_DIR)/musl-strtod.o \
	$(DYNAMIC_SOFTFLOAT_DIR)/musl-compat.o
DYNAMIC_LIBC_OBJS += $(DYNAMIC_GCC_SOFTFLOAT_OBJS) \
	$(DYNAMIC_MUSL_MATH_OBJS) $(DYNAMIC_MUSL_SCAN_OBJS)

$(DYNAMIC_DIR)/obj/%.o: %.c
	@mkdir -p $(dir $@)
	$(CC) $(DYNAMIC_CPPFLAGS) $(DYNAMIC_CFLAGS) -MMD -MP -c $< -o $@

$(DYNAMIC_DIR)/obj/userland/libc/syscall.o: userland/libc/syscall-i386.S
	@mkdir -p $(dir $@)
	$(CC) -m32 -c $< -o $@

$(DYNAMIC_DIR)/obj/userland/rtld/entry.o: userland/rtld/entry-i386.S
	@mkdir -p $(dir $@)
	$(CC) -m32 -c $< -o $@

$(DYNAMIC_SOFTFLOAT_DIR)/gcc-%.o: $(ZEDBSD_GCC_ROOT)/libgcc/soft-fp/%.c
	@mkdir -p $(dir $@)
	$(CC) $(ZEDBSD_GCC_SOFTFP_CPPFLAGS) $(DYNAMIC_CFLAGS) \
		-mlong-double-64 -Wno-error=type-limits -c $< -o $@

$(DYNAMIC_SOFTFLOAT_DIR)/musl-%.o: $(ZEDBSD_MUSL_ROOT)/src/math/%.c
	@mkdir -p $(dir $@)
	$(CC) $(ZEDBSD_MUSL_CPPFLAGS) $(DYNAMIC_CFLAGS) -mlong-double-64 \
		-Wno-error=unused-but-set-variable -Wno-error=parentheses \
		-c $< -o $@

$(DYNAMIC_SOFTFLOAT_DIR)/musl-shgetc.o: \
	$(ZEDBSD_MUSL_ROOT)/src/internal/shgetc.c softfloat/musl-floatscan.h
	@mkdir -p $(dir $@)
	$(CC) $(ZEDBSD_MUSL_CPPFLAGS) $(DYNAMIC_CFLAGS) -mlong-double-64 \
		-Wno-error=parentheses -include softfloat/musl-floatscan.h \
		-c $< -o $@

$(DYNAMIC_SOFTFLOAT_DIR)/musl-floatscan.o: \
	$(ZEDBSD_MUSL_ROOT)/src/internal/floatscan.c softfloat/musl-floatscan.h
	@mkdir -p $(dir $@)
	$(CC) $(ZEDBSD_MUSL_CPPFLAGS) $(DYNAMIC_CFLAGS) -mlong-double-64 \
		-Wno-error=parentheses -Wno-error=sign-compare \
		-include softfloat/musl-floatscan.h -c $< -o $@

$(DYNAMIC_SOFTFLOAT_DIR)/musl-strtod.o: \
	$(ZEDBSD_MUSL_ROOT)/src/stdlib/strtod.c softfloat/musl-floatscan.h
	@mkdir -p $(dir $@)
	$(CC) $(ZEDBSD_MUSL_CPPFLAGS) $(DYNAMIC_CFLAGS) -mlong-double-64 \
		-include softfloat/musl-floatscan.h -c $< -o $@

$(DYNAMIC_SOFTFLOAT_DIR)/musl-compat.o: softfloat/musl-compat.c \
	softfloat/musl-floatscan.h
	@mkdir -p $(dir $@)
	$(CC) $(ZEDBSD_MUSL_CPPFLAGS) $(DYNAMIC_CFLAGS) -mlong-double-64 \
		-include softfloat/musl-floatscan.h -c $< -o $@

$(DYNAMIC_DIR)/ld.so: $(DYNAMIC_RTLD_OBJS)
	$(LD) -m elf_i386 -shared -Bsymbolic -e _rtld_start --hash-style=sysv \
		-z now -z relro -z separate-code $^ -o $@

$(DYNAMIC_DIR)/libc.so: $(DYNAMIC_LIBC_OBJS)
	$(LD) -m elf_i386 -shared -soname libc.so --hash-style=both -z now \
		-z relro -z separate-code $(USER_STACK_LDFLAGS) $^ -o $@

$(DYNAMIC_DIR)/obj/userland/crt1.o: userland/crt1-i386.S
	@mkdir -p $(dir $@)
	$(CC) -m32 -c $< -o $@

$(DYNAMIC_DIR)/alt/rpathdep.so: \
	$(DYNAMIC_DIR)/obj/userland/tests/rpathdep.o $(DYNAMIC_DIR)/ld.so
	@mkdir -p $(dir $@)
	$(LD) -m elf_i386 -shared -soname rpathdep.so --hash-style=gnu \
		-z now -z relro -z separate-code $< -o $@

$(DYNAMIC_DIR)/tlstest.so: \
	$(DYNAMIC_DIR)/obj/userland/tests/tlstest.o \
	$(DYNAMIC_DIR)/alt/rpathdep.so $(DYNAMIC_DIR)/ld.so
	$(LD) -m elf_i386 -shared -soname tlstest.so --hash-style=gnu \
		-z now -z relro -z separate-code --enable-new-dtags \
		-rpath '$$ORIGIN/alt' \
		$(DYNAMIC_DIR)/obj/userland/tests/tlstest.o \
		-L$(DYNAMIC_DIR)/alt -l:rpathdep.so -o $@

$(DYNAMIC_DIR)/rpathtest.so: \
	$(DYNAMIC_DIR)/obj/userland/tests/rpathtest.o \
	$(DYNAMIC_DIR)/alt/rpathdep.so $(DYNAMIC_DIR)/ld.so
	$(LD) -m elf_i386 -shared -soname rpthtest.so --hash-style=gnu \
		-z now -z relro -z separate-code --disable-new-dtags \
		-rpath '$$ORIGIN/alt' $< -L$(DYNAMIC_DIR)/alt \
		-l:rpathdep.so -o $@

$(DYNAMIC_DIR)/verstest.so: \
	$(DYNAMIC_DIR)/obj/userland/tests/versiontest.o \
	userland/tests/versiontest.map $(DYNAMIC_DIR)/ld.so
	$(LD) -m elf_i386 -shared -soname verstest.so --hash-style=gnu \
		-z now -z relro -z separate-code \
		--version-script=userland/tests/versiontest.map $< -o $@

$(DYNAMIC_DIR)/versuse.so: \
	$(DYNAMIC_DIR)/obj/userland/tests/versionuse.o \
	$(DYNAMIC_DIR)/verstest.so $(DYNAMIC_DIR)/ld.so
	$(LD) -m elf_i386 -shared -soname versuse.so --hash-style=gnu \
		-z now -z relro -z separate-code $< -L$(DYNAMIC_DIR) \
		-l:verstest.so -o $@

$(DYNAMIC_DIR)/dyntest: $(DYNAMIC_DIR)/obj/userland/crt1.o \
	$(DYNAMIC_DIR)/obj/userland/tests/dyntest.o $(DYNAMIC_DIR)/libc.so \
	$(DYNAMIC_DIR)/ld.so $(DYNAMIC_DIR)/tlstest.so \
	$(DYNAMIC_DIR)/versuse.so
	$(CC) -m32 -nostdlib -pie -Wl,--no-relax,--hash-style=sysv,-z,now,-z,relro \
		-Wl,-z,separate-code,-z,stack-size=0x100000,--allow-shlib-undefined \
		-Wl,--dynamic-linker=/lib/ld.so \
		$(DYNAMIC_DIR)/obj/userland/crt1.o \
		$(DYNAMIC_DIR)/obj/userland/tests/dyntest.o \
		-L$(DYNAMIC_DIR) -Wl,-rpath-link,$(DYNAMIC_DIR) -l:libc.so -o $@

dynamic-userland-check: $(DYNAMIC_DIR)/ld.so $(DYNAMIC_DIR)/libc.so \
	$(DYNAMIC_DIR)/dyntest $(DYNAMIC_DIR)/tlstest.so \
	$(DYNAMIC_DIR)/rpathtest.so $(DYNAMIC_DIR)/verstest.so \
	$(DYNAMIC_DIR)/versuse.so scripts/check-dynamic-elf.py
	$(PYTHON) scripts/check-dynamic-elf.py --machine i386 --role interpreter $(DYNAMIC_DIR)/ld.so
	$(PYTHON) scripts/check-dynamic-elf.py --machine i386 --role libc $(DYNAMIC_DIR)/libc.so
	$(PYTHON) scripts/check-dynamic-elf.py --machine i386 --role module $(DYNAMIC_DIR)/tlstest.so
	$(PYTHON) scripts/check-dynamic-elf.py --machine i386 --role rpath-module $(DYNAMIC_DIR)/rpathtest.so
	$(PYTHON) scripts/check-dynamic-elf.py --machine i386 --role version-definition $(DYNAMIC_DIR)/verstest.so
	$(PYTHON) scripts/check-dynamic-elf.py --machine i386 --role version-consumer $(DYNAMIC_DIR)/versuse.so
	$(PYTHON) scripts/check-dynamic-elf.py --machine i386 --role program $(DYNAMIC_DIR)/dyntest
	@echo "zedBSD PC-98 i386 dynamic userland artifacts: PASS"
.PHONY: dynamic-userland-check

$(BUILD)/tests/user-fault.o: tests/user-fault.S
	@mkdir -p $(dir $@)
	$(AS) --32 $< -o $@

$(BUILD)/tests/user-stack.o: tests/user-stack.S
	@mkdir -p $(dir $@)
	$(AS) --32 $< -o $@

$(BUILD)/tests/user-stack-guard.o: tests/user-stack-guard.S
	@mkdir -p $(dir $@)
	$(AS) --32 $< -o $@

$(BUILD)/USER-FAULT.ELF: $(BUILD)/tests/user-fault.o $(PC98)/user-init.ld \
	$(USER_ELF_CHECK)
	$(LD) -m elf_i386 -nostdlib -static -z max-page-size=4096 \
		$(USER_STACK_LDFLAGS) \
		-T $(PC98)/user-init.ld $< -o $@
	$(PYTHON) $(USER_ELF_CHECK) $@

$(BUILD)/USER-SWAP.ELF: $(BUILD)/tests/user-swap.o $(PC98)/user-init.ld \
	$(USER_ELF_CHECK)
	$(LD) -m elf_i386 -nostdlib -static -z max-page-size=4096 \
		$(USER_STACK_LDFLAGS) \
		-T $(PC98)/user-init.ld $< -o $@
	$(PYTHON) $(USER_ELF_CHECK) $@

$(BUILD)/USER-STACK.ELF: $(BUILD)/tests/user-stack.o \
	$(PC98)/user-init.ld $(USER_ELF_CHECK)
	$(LD) -m elf_i386 -nostdlib -static -z max-page-size=4096 \
		$(USER_STACK_LDFLAGS) \
		-T $(PC98)/user-init.ld $< -o $@
	$(PYTHON) $(USER_ELF_CHECK) $@

$(BUILD)/USER-STACK-GUARD.ELF: $(BUILD)/tests/user-stack-guard.o \
	$(PC98)/user-init.ld $(USER_ELF_CHECK)
	$(LD) -m elf_i386 -nostdlib -static -z max-page-size=4096 \
		$(USER_STACK_LDFLAGS) \
		-T $(PC98)/user-init.ld $< -o $@
	$(PYTHON) $(USER_ELF_CHECK) $@

$(BUILD)/stage2.elf: $(STAGE2_OBJS) $(PC98)/stage2.ld
	$(LD) -m elf_i386 --gc-sections -z max-page-size=512 \
		-T $(PC98)/stage2.ld -nostdlib \
		$(STAGE2_OBJS) -o $@

# vmunix is the two-segment ELF itself; patch-stage2.py enforces the
# subset contract Stage 1 relies on and patches the B98S v2 header.
$(BUILD)/vmunix: $(BUILD)/stage2.elf $(SCRIPTS_DIR)/patch-stage2.py
	cp $< $@
	$(PYTHON) $(SCRIPTS_DIR)/patch-stage2.py $@

$(BUILD)/stage2-m9-test.elf: $(M9_STAGE2_OBJS) $(PC98)/stage2.ld
	$(LD) -m elf_i386 --gc-sections -z max-page-size=512 \
		-T $(PC98)/stage2.ld -nostdlib \
		$(M9_STAGE2_OBJS) -o $@

$(BUILD)/vmunix-m9: $(BUILD)/stage2-m9-test.elf $(SCRIPTS_DIR)/patch-stage2.py
	cp $< $@
	$(PYTHON) $(SCRIPTS_DIR)/patch-stage2.py $@

$(BUILD)/applet-test.elf: $(BUILD)/$(BOOTSECT)/applet-test.o $(PC98)/applet.ld
	$(LD) -m elf_i386 -T $(PC98)/applet.ld -nostdlib $< -o $@

$(BUILD)/BOOTAPP.BIN: $(BUILD)/applet-test.elf $(SCRIPTS_DIR)/patch-applet.py
	$(OBJCOPY) -O binary $< $@
	$(PYTHON) $(SCRIPTS_DIR)/patch-applet.py $@

# ----------------------------------------------------------------------
# Test disk images.

$(BUILD)/hdd-test.img: all $(SCRIPTS_DIR)/make-hdd-image.sh \
	$(SCRIPTS_DIR)/install-image.sh
	rm -f $@
	$(SCRIPTS_DIR)/make-hdd-image.sh $@

hdd-image: $(BUILD)/hdd-image.img

hdd-boot-qemu-test:
	$(SCRIPTS_DIR)/test-hdd-bare.sh

.PHONY: hdd-image hdd-boot-qemu-test

# ----------------------------------------------------------------------
# PC-98 host tests.

# Register-level backend tests, upstream with the drivers they cover.
$(BUILD)/tests/beui-pc98-gdc-host-test: \
	$(NOCT_ROOT)/tests/testcases/beui-pc98-gdc-test.c \
	$(NOCT_ROOT)/src/api/beui-pc98-gdc.c $(BEUI_CORE_SOURCES)
	@mkdir -p $(dir $@)
	$(BEUI_TEST_CC) $(NOCT_ROOT)/src/api/beui-pc98-gdc.c \
		$(BEUI_CORE_SOURCES) $< -o $@

$(BUILD)/tests/beui-pc98-cirrus-host-test: \
	$(NOCT_ROOT)/tests/testcases/beui-pc98-cirrus-test.c \
	$(NOCT_ROOT)/src/api/beui-pc98-cirrus.c
	@mkdir -p $(dir $@)
	$(BEUI_TEST_CC) $(NOCT_ROOT)/src/api/beui-pc98-cirrus.c $< -o $@

$(BUILD)/tests/noct-host-test: tests/noct-host-test.c \
	tests/vfs-host-stubs.c \
	apps/ls.nct apps/cp.nct userland/noct/integration/noct-m6-script.h \
	$(NOCT_GLUE_OBJS) $(BUILD)/src/kern/env.o $(BUILD)/src/kern/fs.o \
	$(BUILD)/src/kern/namespace.o \
	$(BUILD)/src/kern/buf.o \
	$(BUILD)/src/kern/disk.o $(BUILD)/src/kern/inode.o \
	$(BUILD)/src/kern/file.o $(BUILD)/src/kern/namecache.o \
	$(BUILD)/src/kern/namei.o $(BUILD)/src/kern/mount.o \
	$(BUILD)/src/kern/rootfs.o \
	$(NOCT_OBJECTS) $(ZEDBSD_LIBC_OBJECTS) $(ZEDBSD_SOFTFLOAT_OBJECTS)
	@mkdir -p $(dir $@)
	$(HOSTCC) -m32 -no-pie -fno-builtin -fno-stack-protector -Wall -Wextra \
		-Werror -I. -Iinclude -Isrc -Ilibc/include -I$(NOCT_ROOT)/include \
		-DZEDBSD_NOCT_JIT_CODE_MAX=$(NOCT_JIT_CODE_MAX) \
		tests/noct-host-test.c tests/vfs-host-stubs.c $(NOCT_GLUE_OBJS) \
		$(BUILD)/src/kern/env.o $(BUILD)/src/kern/fs.o \
		$(BUILD)/src/kern/namespace.o $(NOCT_OBJECTS) \
		$(BUILD)/src/kern/buf.o \
		$(BUILD)/src/kern/disk.o $(BUILD)/src/kern/inode.o \
		$(BUILD)/src/kern/file.o $(BUILD)/src/kern/namecache.o \
		$(BUILD)/src/kern/namei.o $(BUILD)/src/kern/mount.o \
		$(BUILD)/src/kern/rootfs.o \
		$(ZEDBSD_LIBC_OBJECTS) $(ZEDBSD_SOFTFLOAT_OBJECTS) -o $@

NOCT_M6_JIT_CODE := $(BUILD)/logs/m6-jit-code.bin
NOCT_TEST_JIT_CODE_SIZE := 98304

noct-host-test: $(BUILD)/tests/noct-host-test
	@mkdir -p $(dir $(NOCT_M6_JIT_CODE))
	$(BUILD)/tests/noct-host-test $(NOCT_M6_JIT_CODE) apps/ls.nct apps/cp.nct
	@test $$(stat -c%s $(NOCT_M6_JIT_CODE)) -eq $(NOCT_TEST_JIT_CODE_SIZE)
	@echo "zedBSD Noct interpreter/JIT lifecycle host tests: PASS"

# Compile-check the i386 HAL and PC-98 BSP under the same freestanding
# target flags used by the vmunix link.

$(BUILD)/src/hal/%.o: src/hal/%.c
	@mkdir -p $(dir $@)
	$(HAL_CC) -MMD -MP -c $< -o $@

$(BUILD)/src/hal/%.o: src/hal/%.S
	@mkdir -p $(dir $@)
	$(HAL_CC) -D_ASM_SRC_ -MMD -MP -c $< -o $@

hal-pc98-compile: $(HAL_PC98_OBJS)
	@echo "HAL i386/PC-98 compile check: PASS"

# The kernel-side HAL glue compiles in zedBSD' own type world.

$(BUILD)/src/kern/entry.o: src/kern/entry.c
	@mkdir -p $(dir $@)
	$(ZEDBSD_KERN_CC) -MMD -MP -c $< -o $@

kern-compile: $(KERN_OBJS)
	@echo "zedBSD kernel glue compile check: PASS"
.PHONY: hal-pc98-compile kern-compile

$(BUILD)/tests/hal-pc98-keyboard-host-test: \
	tests/hal-pc98-keyboard-host-test.c \
	src/hal/i386/bsp-pc98/cons.c \
	src/hal/i386/bsp-pc98/jisx0208.c
	@mkdir -p $(dir $@)
	$(HOST_TEST_CC) -std=gnu11 -Iinclude -Isrc $< -o $@

HOST_TEST_BINARIES += $(BUILD)/tests/beui-pc98-gdc-host-test \
	$(BUILD)/tests/beui-pc98-cirrus-host-test \
	$(BUILD)/tests/hal-pc98-keyboard-host-test
CHECK_RUN_TARGETS += noct-host-test hal-pc98-compile kern-compile

# ----------------------------------------------------------------------
# Milestone and QEMU verification chains.

noct-m4-opcode-check: $(BUILD)/userland/noct/integration/noct.o $(BUILD)/userland/noct/integration/platform.o
	@if $(NOCT_OBJDUMP) -d --no-show-raw-insn $^ | \
		grep -E '(^[[:space:]]*[0-9a-f]+:[[:space:]]+f[a-z0-9]+[[:space:]])|\b(bswap|cmpxchg|xadd|cmov[a-z]*|rdtsc|ud2|cpuid|fx[a-z]+|movaps|movups|xmm[0-9]|ymm[0-9]|zmm[0-9])\b'; then \
		echo "ERROR: M4 glue contains a post-i386 opcode" >&2; \
		exit 1; \
	fi
	@echo "zedBSD Noct M4 glue i386 opcode check: PASS"

noct-m4-verify: noct-m3-verify noct-host-test noct-m4-opcode-check \
	$(BUILD)/vmunix
	@echo "zedBSD M4 historical lifecycle checks: PASS"

NOCT_M5_DISASSEMBLY := $(BUILD)/logs/m5.disassembly
NOCT_M5_REJECTED := $(BUILD)/logs/m5-rejected.txt

noct-m5-final-opcode-check: $(BUILD)/stage2.elf softfloat-opcode-check
	@mkdir -p $(dir $(NOCT_M5_DISASSEMBLY))
	@$(NOCT_OBJDUMP) -d --no-show-raw-insn $(BUILD)/stage2.elf > \
		$(NOCT_M5_DISASSEMBLY)
	@# i486DX task switching legitimately uses the 80387-compatible
	@# fnsave/frstor pair. SSE, FXSR, XMM and all other x87 instructions
	@# remain forbidden in the linked image.
	@grep -E '(^[[:space:]]*[0-9a-f]+:[[:space:]]+f[a-z0-9]+[[:space:]])|\b(bswap|cmpxchg|xadd|cmov[a-z]*|rdtsc|ud2|cpuid|fx[a-z]+|movaps|movups|xmm[0-9]|ymm[0-9]|zmm[0-9])\b' \
		$(NOCT_M5_DISASSEMBLY) | \
		grep -Ev '^[[:space:]]*[0-9a-f]+:[[:space:]]+(fnsave|frstor)[[:space:]]' \
		> $(NOCT_M5_REJECTED) || true
	@if test -s $(NOCT_M5_REJECTED); then \
		echo "ERROR: final vmunix ELF contains a forbidden opcode" >&2; \
		cat $(NOCT_M5_REJECTED) >&2; \
		exit 1; \
	fi
	@echo "zedBSD M5 final i386 opcode check: PASS"

noct-m5-verify: noct-m4-verify softfloat-host-test noct-m5-final-opcode-check
	@echo "zedBSD M5 historical soft-float checks: PASS"

noct-m6-verify: noct-m5-verify noct-host-test $(BUILD)/vmunix
	@echo "zedBSD M6 verification: PASS (forced i386 JIT)"

noct-m7-verify: noct-m6-verify noct-host-test $(BUILD)/vmunix
	@echo "zedBSD M7 host/build verification: PASS (arguments and main signature)"

noct-m8-verify: noct-m7-verify noct-host-test $(BUILD)/vmunix \
	noct-m5-final-opcode-check
	@echo "zedBSD M8 host/build verification: PASS (safe native APIs)"

bios-write-qemu-test: $(BUILD)/vmunix-m9
	$(SCRIPTS_DIR)/test-bios-write.sh all

noct-m9-verify: noct-m8-verify check $(BUILD)/vmunix $(BUILD)/vmunix-m9 \
	bios-write-qemu-test
	@echo "zedBSD M9 verification: PASS (IDE/SCSI BIOS write/read/restore)"

noct-file-qemu-test: $(BUILD)/vmunix
	$(SCRIPTS_DIR)/test-noct-file.sh

ide-multidrive-qemu-test: $(BUILD)/vmunix
	$(SCRIPTS_DIR)/test-ide-multidrive.sh

noct-m10-verify: noct-m9-verify check $(BUILD)/vmunix \
	noct-m5-final-opcode-check noct-file-qemu-test
	@echo "zedBSD M10 verification: PASS (FAT16 writer and Noct File API)"

noct-utilities-qemu-test: $(BUILD)/vmunix apps/ls.nct apps/cp.nct
	$(SCRIPTS_DIR)/test-noct-utilities.sh

sh-builtins-qemu-test: $(BUILD)/vmunix $(BUILD)/bin/sh bios-bootloader
	$(SCRIPTS_DIR)/test-sh-builtins.sh pc98

noct-m11-verify: noct-m10-verify check $(BUILD)/vmunix \
	noct-m5-final-opcode-check noct-utilities-qemu-test
	@echo "zedBSD M11 safe utilities verification: PASS (ls.nct and cp.nct)"

noct-env-qemu-test: $(BUILD)/vmunix
	$(SCRIPTS_DIR)/test-noct-env.sh

noct-m14-verify: noct-m11-verify check $(BUILD)/vmunix \
	noct-m5-final-opcode-check noct-env-qemu-test
	@echo "zedBSD M14 verification: PASS (environment and intrinsic APIs)"

noct-repl-qemu-test: $(BUILD)/vmunix
	$(SCRIPTS_DIR)/test-noct-repl.sh

term-japanese-qemu-test: $(BUILD)/vmunix
	$(SCRIPTS_DIR)/test-term-japanese.sh

noct-m15-verify: noct-m14-verify check $(BUILD)/vmunix \
	noct-m5-final-opcode-check noct-repl-qemu-test
	@echo "zedBSD M15 REPL verification: PASS (keyboard/error/Ctrl-C on i386)"

noct-m17-verify: noct-m15-verify check $(BUILD)/vmunix \
	noct-m5-final-opcode-check
	@for memory in 5 8 16 32 64 96; do \
		echo "Testing zedBSD Noct RAM profile: $${memory} MiB"; \
		ZEDBSD_TEST_MEMORY_MIB=$$memory \
			$(SCRIPTS_DIR)/test-noct-repl.sh || exit $$?; \
	done
	@echo "zedBSD M17 verification: PASS (5/8/16/32/64/>64 MiB profiles)"

beui-g1-verify: check $(BUILD)/vmunix noct-m5-final-opcode-check
	@echo "zedBSD BeUI G1 verification: PASS (lifecycle and HAL boundary)"

beui-gdc-qemu-test: $(BUILD)/vmunix
	$(SCRIPTS_DIR)/test-beui-gdc.sh

beui-g2a-verify: check $(BUILD)/vmunix noct-m5-final-opcode-check \
	beui-gdc-qemu-test
	@echo "zedBSD BeUI G2a verification: PASS (GDC and BMP image path)"

beui-cirrus-qemu-test: $(BUILD)/vmunix
	$(SCRIPTS_DIR)/test-beui-cirrus.sh

beui-g2b-verify: beui-g2a-verify
	$(SCRIPTS_DIR)/test-beui-cirrus.sh
	@echo "zedBSD BeUI G2b verification: PASS (Core-Graph/Cirrus and GDC fallback)"

beui-menu-cirrus-qemu-test: $(BUILD)/vmunix
	$(SCRIPTS_DIR)/test-beui-menu.sh

beui-menu-gdc-qemu-test: $(BUILD)/vmunix
	ZEDBSD_BEUI_MACHINE=pc9801 ZEDBSD_BEUI_TEST_TAG=menu-gdc \
		$(SCRIPTS_DIR)/test-beui-menu.sh

beui-g2c-verify: beui-g2b-verify \
	beui-menu-cirrus-qemu-test beui-menu-gdc-qemu-test
	@echo "zedBSD BeUI G2c verification: PASS (CGROM text and keyboard menu)"

beui-g4-verify: beui-g2c-verify term-japanese-qemu-test
	@echo "zedBSD BeUI G4 verification: PASS (user-process menu and Japanese text)"

beui-input-qemu-test: $(BUILD)/vmunix
	$(SCRIPTS_DIR)/test-beui-input.sh

swap-lowmem-qemu-test: $(BUILD)/vmunix $(BUILD)/USER-SWAP.ELF
	$(SCRIPTS_DIR)/test-swap-lowmem.sh

beui-g5-verify: beui-g4-verify beui-input-qemu-test
	@echo "zedBSD BeUI G5 verification: PASS (BeUI-only input)"

.PHONY: noct-host-test noct-m4-opcode-check noct-m4-verify \
	ide-multidrive-qemu-test \
	noct-m5-final-opcode-check noct-m5-verify noct-m6-verify \
	noct-m7-verify noct-m8-verify bios-write-qemu-test noct-m9-verify \
	noct-file-qemu-test noct-m10-verify noct-utilities-qemu-test \
	sh-builtins-qemu-test \
	noct-m11-verify noct-env-qemu-test noct-m14-verify \
	noct-repl-qemu-test term-japanese-qemu-test noct-m15-verify \
	noct-m17-verify beui-g1-verify beui-gdc-qemu-test beui-g2a-verify \
	beui-cirrus-qemu-test beui-g2b-verify beui-menu-cirrus-qemu-test \
	beui-menu-gdc-qemu-test beui-g2c-verify beui-g4-verify \
	beui-input-qemu-test beui-g5-verify swap-lowmem-qemu-test

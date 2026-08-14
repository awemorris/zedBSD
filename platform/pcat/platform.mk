# zedBSD IBM PC/AT i386 platform rules.
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
PCAT := platform/pcat
BOOTSECT := bootsectors/pcat
BIOS_LOADER := bootloader/pcat

HAL_CC := $(CC) -m32 -march=i386 -ffreestanding -fno-pic -fno-pie \
	-fno-stack-protector -nostdinc -Os -Wall -Wextra -Werror \
	-Iinclude -Iinclude/uapi -Isrc -Isrc/hal/i386 -Ilibc/include \
	-DHAL_ARCH_I386 -DHAL_BOARD_PCAT -DHAL_PCAT_DEBUGCON
HAL_PCAT_SOURCES := src/hal/i386/lib.c src/hal/i386/irq.c \
	src/hal/i386/page.c src/hal/i386/space.c src/hal/i386/int.c \
	src/hal/i386/cmain.c src/hal/i386/task.c src/hal/i386/fb.c \
	src/hal/i386/bsp-pcat/boot.c src/hal/i386/bsp-pcat/cons.c \
	src/hal/i386/bsp-pcat/pic.c src/hal/i386/bsp-pcat/clock.c
HAL_PCAT_ASM := src/hal/i386/locore.S src/hal/i386/trap.S \
	src/hal/i386/dispatch.S
HAL_PCAT_OBJS := $(patsubst %.c,$(BUILD)/%.o,$(HAL_PCAT_SOURCES)) \
	$(patsubst %.S,$(BUILD)/%.o,$(HAL_PCAT_ASM))

ZEDBSD_KERN_CC := $(CC) -m32 -march=i386 -ffreestanding -fno-pic -fno-pie \
	-fno-stack-protector -nostdinc -Os -Wall -Wextra -Werror \
	-Iinclude -Iinclude/uapi -Isrc -I. -Ilibc/include
KERN_OBJS := $(BUILD)/src/kern/entry.o $(BUILD)/src/kern/clock.o \
	$(BUILD)/src/kern/process.o $(BUILD)/src/kern/thread.o \
	$(BUILD)/src/kern/sched.o $(BUILD)/src/kern/vmspace.o \
	$(BUILD)/src/kern/vm-commit.o $(BUILD)/src/kern/filedesc.o \
	$(BUILD)/src/kern/cwdinfo.o $(BUILD)/src/kern/elf.o \
	$(BUILD)/src/kern/exec.o $(BUILD)/src/kern/user-probe.o \
	$(BUILD)/src/kern/syscall.o $(BUILD)/src/kern/uaccess.o \
	$(BUILD)/src/kern/cdev.o $(BUILD)/src/kern/devfs.o \
	$(BUILD)/src/kern/console-device.o $(BUILD)/src/kern/graphics-device.o \
	$(BUILD)/src/kern/system-device.o \
	$(BUILD)/src/kern/pcat/font.o $(BUILD)/src/kern/pcat/graphics.o \
	$(BUILD)/src/kern/pcat/unsupported-devices.o $(BUILD)/src/kern/init.o

VMUNIX_OBJS := $(BUILD)/src/kern/main.o $(BUILD)/src/kern/env.o \
	$(BUILD)/src/kern/fs.o $(BUILD)/src/kern/namespace.o \
	$(BUILD)/src/kern/fat.o $(BUILD)/src/kern/fat-lfn.o \
	$(BUILD)/src/kern/fat16.o $(BUILD)/src/kern/fat-vfs.o \
	$(BUILD)/src/kern/inode.o $(BUILD)/src/kern/file.o \
	$(BUILD)/src/kern/namecache.o $(BUILD)/src/kern/namei.o \
	$(BUILD)/src/kern/mount.o $(BUILD)/src/kern/rootfs.o \
	$(BUILD)/src/kern/vfs.o $(BUILD)/src/kern/swap.o \
	$(BUILD)/src/kern/swap-fat.o $(BUILD)/src/kern/vm-reclaim.o \
	$(BUILD)/src/kern/disk.o $(BUILD)/src/kern/partition.o \
	$(BUILD)/drivers/pcat-ide.o $(BUILD)/src/kern/mbr-partition.o \
	$(BUILD)/src/kern/pcat/platform.o $(BUILD)/src/kern/image.o \
	$(BUILD)/src/kern/panic.o $(ZEDBSD_LIBC_OBJECTS) \
	$(HAL_PCAT_OBJS) $(KERN_OBJS)

all: $(BUILD)/bootsect.bin $(BUILD)/vmunix $(BUILD)/bin/sh \
	$(BUILD)/bin/noct $(BUILD)/hdd-image.img \
	$(BUILD)/zedbsd-grub.iso
vmunix: $(BUILD)/vmunix
SH: $(BUILD)/bin/sh
NOCT.ELF: $(BUILD)/NOCT.ELF
.PHONY: vmunix SH NOCT.ELF

$(BUILD)/$(BOOTSECT)/bootsect.o: $(BOOTSECT)/bootsect.S
	@mkdir -p $(dir $@)
	$(CC) -m32 -c $< -o $@
$(BUILD)/bootsect.elf: $(BUILD)/$(BOOTSECT)/bootsect.o \
	$(BOOTSECT)/bootsect.ld
	$(LD) -m elf_i386 -T $(BOOTSECT)/bootsect.ld $< -o $@
$(BUILD)/bootsect.bin: $(BUILD)/bootsect.elf
	$(OBJCOPY) -O binary -j .text $< $@
	@test $$(stat -c%s $@) -eq 512
	@test "$$(od -An -tx1 -j510 -N2 $@ | tr -d ' \n')" = 55aa

# Native two-stage MBR/FAT16 loader.  Keep the historical raw-LBA loader
# above until the new path has completed its regression matrix.
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
	bootloader/include/elf.inc bootloader/include/amd64-handoff.h
	@mkdir -p $(dir $@)
	$(CC) -m64 -I. -x assembler-with-cpp -c $< -o $@

$(BUILD)/bootloader/stage2.elf: $(BUILD)/bootloader/stage2.o \
	$(BIOS_LOADER)/stage2.ld
	$(LD) -m elf_x86_64 -T $(BIOS_LOADER)/stage2.ld $< -o $@

$(BUILD)/bootloader/stage2.raw: $(BUILD)/bootloader/stage2.elf
	$(OBJCOPY) -O binary -j .text $< $@

$(BUILD)/bootloader/stage2.bin: $(BUILD)/bootloader/stage2.raw \
	$(SCRIPTS_DIR)/finalize-bios-stage2.py
	$(PYTHON) $(SCRIPTS_DIR)/finalize-bios-stage2.py --machine pcat $< $@

$(BUILD)/bootloader/payload32.o: bootloader/tests/payload32-pcat.S
	@mkdir -p $(dir $@)
	$(CC) -m32 -c $< -o $@

$(BUILD)/bootloader/payload32.elf: $(BUILD)/bootloader/payload32.o \
	bootloader/tests/payload32-pcat.ld
	$(LD) -m elf_i386 -T bootloader/tests/payload32-pcat.ld $< -o $@

$(BUILD)/bootloader/payload64.o: bootloader/tests/payload64-pcat.S \
	bootloader/include/amd64-handoff.h
	@mkdir -p $(dir $@)
	$(CC) -m64 -I. -c $< -o $@

$(BUILD)/bootloader/payload64.elf: $(BUILD)/bootloader/payload64.o \
	bootloader/tests/payload64-pcat.ld
	$(LD) -m elf_x86_64 -T bootloader/tests/payload64-pcat.ld $< -o $@

bios-bootloader: $(BUILD)/bootloader/stage1.bin \
	$(BUILD)/bootloader/stage2.bin

$(BUILD)/bios-hdd-image.img: $(BUILD)/bootloader/stage1.bin \
	$(BUILD)/bootloader/stage2.bin $(BUILD)/vmunix $(BUILD)/bin/sh \
	$(BUILD)/bin/noct $(HOLORIS_NOCT) \
	$(SCRIPTS_DIR)/make-bios-hdd-image.py \
	$(SCRIPTS_DIR)/check-bios-hdd-image.py
	$(PYTHON) $(SCRIPTS_DIR)/make-bios-hdd-image.py --force \
		--machine pcat --stage1 $(BUILD)/bootloader/stage1.bin \
		--stage2 $(BUILD)/bootloader/stage2.bin --kernel $(BUILD)/vmunix \
		--shell $(BUILD)/bin/sh --noct $(BUILD)/bin/noct \
		--holoris $(HOLORIS_NOCT) $@

bios-hdd-image: $(BUILD)/bios-hdd-image.img
bios-loader-host-check: $(BUILD)/bios-hdd-image.img
	$(PYTHON) $(SCRIPTS_DIR)/check-bios-hdd-image.py --machine pcat \
		--kernel $(BUILD)/vmunix --noct $(BUILD)/bin/noct \
		--holoris $(HOLORIS_NOCT) $<

bios-loader-qemu-test: bios-bootloader \
	$(BUILD)/bootloader/payload32.elf $(BUILD)/bootloader/payload64.elf
	bash $(SCRIPTS_DIR)/test-bios-bootloader-qemu.sh pcat

.PHONY: bios-bootloader bios-hdd-image bios-loader-host-check \
	bios-loader-qemu-test

$(BUILD)/src/hal/%.o: src/hal/%.c
	@mkdir -p $(dir $@)
	$(HAL_CC) -MMD -MP -c $< -o $@
$(BUILD)/src/hal/%.o: src/hal/%.S
	@mkdir -p $(dir $@)
	$(HAL_CC) -D_ASM_SRC_ -MMD -MP -c $< -o $@
$(BUILD)/src/kern/entry.o: src/kern/entry.c
	@mkdir -p $(dir $@)
	$(ZEDBSD_KERN_CC) -MMD -MP -c $< -o $@

$(BUILD)/vmunix: $(VMUNIX_OBJS) $(PCAT)/vmunix.ld \
	$(SCRIPTS_DIR)/check-pcat-vmunix.py
	$(LD) -m elf_i386 --gc-sections -z max-page-size=4096 \
		-T $(PCAT)/vmunix.ld -nostdlib $(VMUNIX_OBJS) -o $@
	$(PYTHON) $(SCRIPTS_DIR)/check-pcat-vmunix.py $@
	grub-file --is-x86-multiboot $@

USER_LIBC_OBJS := $(BUILD)/userland/crt0.o \
	$(BUILD)/userland/libc/posix.o $(BUILD)/libc/heap.o \
	$(BUILD)/libc/string.o $(BUILD)/libc/ctype.o $(BUILD)/libc/int64.o \
	$(BUILD)/libc/strto.o $(BUILD)/libc/format.o $(BUILD)/libc/stdio.o
USER_CFLAGS := $(ZEDBSD_CFLAGS) -fno-builtin -ffunction-sections \
	-fdata-sections -msoft-float -mno-80387 -mno-fp-ret-in-387 \
	-mno-mmx -mno-sse -mno-sse2
USER_STACK_LDFLAGS := -z stack-size=0x100000
USER_ELF_CHECK := $(SCRIPTS_DIR)/check-user-elf.py
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
	$(USER_NOCT_OBJECTS) $(ZEDBSD_SOFTFLOAT_OBJECTS) $(PCAT)/user.ld \
	$(USER_ELF_CHECK)
	$(LD) -m elf_i386 --gc-sections -nostdlib -static -z max-page-size=4096 \
		$(USER_STACK_LDFLAGS) -T $(PCAT)/user.ld $(USER_LIBC_OBJS) \
		$(USER_NOCT_GLUE_OBJS) $(USER_NOCT_OBJECTS) \
		$(ZEDBSD_SOFTFLOAT_OBJECTS) -o $@
	@test -z "$$($(NOCT_NM) -u $@)" || { $(NOCT_NM) -u $@; exit 1; }
	$(PYTHON) $(USER_ELF_CHECK) $@

$(BUILD)/bin/noct: $(BUILD)/NOCT.ELF
	@mkdir -p $(dir $@)
	cp $< $@
	$(PYTHON) $(USER_ELF_CHECK) $@

USER_SH_OBJS := $(BUILD)/userland/sh/main.o $(BUILD)/userland/sh/applet.o \
	$(BUILD)/userland/sh/builtins.o
$(BUILD)/userland/libc/posix.o $(USER_SH_OBJS): OBJ_CPPFLAGS = $(ZEDBSD_CPPFLAGS)
$(BUILD)/userland/libc/posix.o $(USER_SH_OBJS): OBJ_CFLAGS = $(USER_CFLAGS)

$(BUILD)/bin/sh: $(USER_LIBC_OBJS) $(USER_SH_OBJS) \
	$(ZEDBSD_SOFTFLOAT_OBJECTS) $(PCAT)/user.ld $(USER_ELF_CHECK)
	@mkdir -p $(dir $@)
	$(LD) -m elf_i386 --gc-sections -nostdlib -static -z max-page-size=4096 \
		$(USER_STACK_LDFLAGS) -T $(PCAT)/user.ld $(USER_LIBC_OBJS) \
		$(USER_SH_OBJS) $(ZEDBSD_SOFTFLOAT_OBJECTS) -o $@
	@test -z "$$($(NOCT_NM) -u $@)" || { $(NOCT_NM) -u $@; exit 1; }
	$(PYTHON) $(USER_ELF_CHECK) $@

$(BUILD)/legacy-pcat-hdd-image.img: $(BUILD)/bootsect.bin $(BUILD)/vmunix $(BUILD)/bin/sh \
	$(SCRIPTS_DIR)/make-pcat-hdd-image.sh
	$(SCRIPTS_DIR)/make-pcat-hdd-image.sh $@ $(BUILD)/bootsect.bin \
		$(BUILD)/vmunix $(BUILD)/bin/sh

$(BUILD)/hdd-image.img: $(BUILD)/bios-hdd-image.img
	cp -f $< $@

$(BUILD)/zedbsd-grub.iso: $(BUILD)/vmunix \
	$(SCRIPTS_DIR)/make-pcat-grub-iso.sh
	$(SCRIPTS_DIR)/make-pcat-grub-iso.sh $@ $(BUILD)/vmunix

hdd-image: $(BUILD)/hdd-image.img
grub-iso: $(BUILD)/zedbsd-grub.iso
.PHONY: hdd-image grub-iso

$(BUILD)/tests/pcat-mbr-host-test: tests/pcat-mbr-host-test.c \
	src/kern/disk.c src/kern/partition.c src/kern/mbr-partition.c
	@mkdir -p $(dir $@)
	$(HOST_TEST_CC) -Iinclude -Isrc src/kern/disk.c src/kern/partition.c \
		src/kern/mbr-partition.c $< -o $@

pcat-mbr-host-test: $(BUILD)/tests/pcat-mbr-host-test
	$(BUILD)/tests/pcat-mbr-host-test

hdd-boot-qemu-test: $(BUILD)/vmunix $(BUILD)/hdd-image.img \
	$(BUILD)/zedbsd-grub.iso
	$(SCRIPTS_DIR)/test-pcat-qemu.sh $(BUILD)/vmunix \
		$(BUILD)/hdd-image.img $(BUILD)/zedbsd-grub.iso

sh-builtins-qemu-test: $(BUILD)/vmunix $(BUILD)/bin/sh bios-bootloader
	$(SCRIPTS_DIR)/test-sh-builtins.sh pcat

pcat-beui-qemu-test: $(BUILD)/vmunix $(BUILD)/bin/noct \
	build/unified/hdd-image.img
	bash $(SCRIPTS_DIR)/test-pcat-beui.sh build/unified/hdd-image.img

HOST_TEST_BINARIES += $(BUILD)/tests/pcat-mbr-host-test
.PHONY: pcat-mbr-host-test hdd-boot-qemu-test sh-builtins-qemu-test \
	pcat-beui-qemu-test

hal-pcat-compile: $(HAL_PCAT_OBJS)
	@echo "HAL i386/PCAT compile check: PASS"
kern-compile: $(KERN_OBJS)
	@echo "zedBSD kernel glue compile check: PASS"
CHECK_RUN_TARGETS += hal-pcat-compile kern-compile
.PHONY: hal-pcat-compile kern-compile

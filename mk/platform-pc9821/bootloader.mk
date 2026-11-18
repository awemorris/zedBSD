# zedBSD
# Copyright (C) 2026 Awe Morris
#
# SPDX-License-Identifier: Zlib

#
# PC-98 bootloader build rules
#

BIOS_LOADER := bootloader/pc98

.PHONY: bootloader-platform
bootloader-platform: $(BUILD)/bootloader/stage1.bin \
                     $(BUILD)/bootloader/stage2.bin \
                     $(BUILD)/bootloader/partition-pbr.bin \
                     $(BUILD)/bootloader/BOOTZBSD.EXE

# Stage 1: IPL at sector 0 (disk-ipl.S)
$(BUILD)/bootloader/stage1.o: $(BIOS_LOADER)/disk-ipl.S
	@mkdir -p $(dir $@)
	$(CC) -m32 -I. -x assembler-with-cpp -c $< -o $@

$(BUILD)/bootloader/stage1.elf: $(BUILD)/bootloader/stage1.o $(BIOS_LOADER)/stage1.ld
	$(LD) -m elf_i386 -T $(BIOS_LOADER)/stage1.ld $< -o $@

$(BUILD)/bootloader/stage1.bin: $(BUILD)/bootloader/stage1.elf
	$(OBJCOPY) -O binary -j .text $< $@
	@test $$(stat -c%s $@) -eq 512
	@test "$$(od -An -tx1 -j510 -N2 $@ | tr -d ' \n')" = 55aa

# Stage 2: LBA2 loader (lba2.S)
$(BUILD)/bootloader/stage2.o: $(BIOS_LOADER)/lba2.S
	@mkdir -p $(dir $@)
	$(CC) -m32 -I. -x assembler-with-cpp -c $< -o $@

$(BUILD)/bootloader/stage2.elf: $(BUILD)/bootloader/stage2.o
	$(LD) -m elf_i386 --image-base=0 -Ttext=0 -e _start $< -o $@

$(BUILD)/bootloader/stage2.bin: $(BUILD)/bootloader/stage2.elf
	$(OBJCOPY) -O binary -j .text $< $@
	@test $$(stat -c%s $@) -eq 7168

# Stage 3: Partition PBR (partition-pbr.S)
$(BUILD)/bootloader/partition-pbr.o: $(BIOS_LOADER)/partition-pbr.S \
	$(BIOS_LOADER)/bootstrap.inc bootloader/include/stage2-header.inc
	@mkdir -p $(dir $@)
	$(CC) -m32 -I. -x assembler-with-cpp -c $< -o $@

$(BUILD)/bootloader/partition-pbr.elf: $(BUILD)/bootloader/partition-pbr.o
	$(LD) -m elf_i386 --image-base=0 -Ttext=0 -e _start $< -o $@

$(BUILD)/bootloader/partition-pbr.bin: $(BUILD)/bootloader/partition-pbr.elf
	$(OBJCOPY) -O binary -j .text $< $@

# Stage 4: BOOTZBSD.EXE
$(BUILD)/bootloader/bootzbsd.o: $(BIOS_LOADER)/bootzbsd.S \
	bootloader/bios/fat-directory.h \
	bootloader/include/disk-layout.inc bootloader/include/stage2-header.inc \
	bootloader/include/mbr.inc bootloader/include/fat16.inc \
	bootloader/include/elf.inc bootloader/include/boot-parameter-handoff.h \
	bootloader/uefi/zedbsd-config.h \
	include/kern/boot.h \
	src/hal/i386/bsp-pc98/handoff.h
	@mkdir -p $(dir $@)
	$(CC) -m32 -I. -x assembler-with-cpp -c $< -o $@

$(BUILD)/bootloader/bios-zedbsd-config.i386.o: bootloader/uefi/zedbsd-config.c \
	bootloader/uefi/zedbsd-config.h bootloader/include/boot-parameter-handoff.h include/kern/boot.h
	@mkdir -p $(dir $@)
	$(CC) -m16 -march=i386 -mtune=i386 -Os -ffreestanding -fno-pic -fno-pie \
		-fno-stack-protector -fno-asynchronous-unwind-tables \
		-fno-unwind-tables -fno-builtin -Wall -Wextra -Werror -I. \
		-c $< -o $@

$(BUILD)/bootloader/bios-fat-directory.i386.o: bootloader/bios/fat-directory.c \
	bootloader/bios/fat-directory.h
	@mkdir -p $(dir $@)
	$(CC) -m16 -march=i386 -mtune=i386 -Os -ffreestanding -fno-pic -fno-pie \
		-fno-stack-protector -fno-asynchronous-unwind-tables \
		-fno-unwind-tables -fno-builtin -Wall -Wextra -Werror -I. \
		-c $< -o $@

PC98_BOOTZBSD_HELPERS := $(BUILD)/bootloader/bios-zedbsd-config.i386.o \
	$(BUILD)/bootloader/bios-fat-directory.i386.o

$(BUILD)/bootloader/bootzbsd.elf: $(BUILD)/bootloader/bootzbsd.o \
	$(PC98_BOOTZBSD_HELPERS) $(BIOS_LOADER)/stage2.ld
	$(LD) -m elf_i386 -T $(BIOS_LOADER)/stage2.ld $(filter %.o,$^) -o $@

$(BUILD)/bootloader/bootzbsd.raw: $(BUILD)/bootloader/bootzbsd.elf
	$(OBJCOPY) -O binary -j .text $< $@

$(BUILD)/bootloader/bootzbsd.bin: $(BUILD)/bootloader/bootzbsd.raw
	$(PYTHON) mk/platform-pc9821/tools/finalize-bios-stage2.py --machine pc98 $< $@

$(BUILD)/bootloader/BOOTZBSD.EXE: $(BUILD)/bootloader/bootzbsd.bin
	$(PYTHON) mk/platform-pc9821/tools/make-mz-exe.py --entry 0x20 $< $@

# Shared PC/AT + PC-98 BIOS loader rules.
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib

PC_UNIFIED := bootloader/pc-unified
PC_UNIFIED_BUILD := build/pc-unified
UEFI_LOADER := bootloader/uefi
UEFI_BUILD := build/uefi
EFI_CC ?= x86_64-w64-mingw32-gcc
EFI_LD ?= x86_64-w64-mingw32-ld
EFI_NM ?= x86_64-w64-mingw32-nm
EFI_CFLAGS := -std=c11 -ffreestanding -fshort-wchar -mno-red-zone \
	-fno-stack-protector -fno-builtin -fno-asynchronous-unwind-tables \
	-fno-unwind-tables -fno-ident -ffunction-sections -fdata-sections \
	-Os -Wall -Wextra -Werror -I.
PC_UNIFIED_COMMON_DEPS := $(PC_UNIFIED)/layout.inc \
	bootloader/include/stage2-header.inc

$(UEFI_BUILD)/bootx64.o: $(UEFI_LOADER)/bootx64.c \
	$(UEFI_LOADER)/include/uefi.h $(UEFI_LOADER)/elf64.h \
	bootloader/include/amd64-handoff.h
	@mkdir -p $(dir $@)
	$(EFI_CC) $(EFI_CFLAGS) -c $< -o $@

$(UEFI_BUILD)/elf64.o: $(UEFI_LOADER)/elf64.c $(UEFI_LOADER)/elf64.h
	@mkdir -p $(dir $@)
	$(EFI_CC) $(EFI_CFLAGS) -c $< -o $@

$(UEFI_BUILD)/transition.o: $(UEFI_LOADER)/transition.S
	@mkdir -p $(dir $@)
	$(EFI_CC) -m64 -mno-red-zone -c $< -o $@

$(UEFI_BUILD)/BOOTX64.EFI: $(UEFI_BUILD)/bootx64.o \
	$(UEFI_BUILD)/elf64.o $(UEFI_BUILD)/transition.o \
	$(SCRIPTS_DIR)/check-bootx64.py
	$(EFI_LD) -mi386pep --subsystem 10 --entry efi_main --image-base 0 \
		--gc-sections --enable-reloc-section --no-insert-timestamp \
		$(filter %.o,$^) -o $@
	@test -z "$$($(EFI_NM) -u $@ | grep -Ev \
		' (__bss_start__|__bss_end__|__end__|___tls_start__|___tls_end__)$$')" \
		|| { $(EFI_NM) -u $@; exit 1; }
	$(PYTHON) $(SCRIPTS_DIR)/check-bootx64.py $@

uefi-loader: $(UEFI_BUILD)/BOOTX64.EFI

uefi-loader-host-check: $(UEFI_BUILD)/BOOTX64.EFI
	$(PYTHON) $(SCRIPTS_DIR)/check-bootx64.py $<

$(PC_UNIFIED_BUILD)/stage0.o: $(PC_UNIFIED)/stage0.S \
	$(PC_UNIFIED)/layout.inc
	@mkdir -p $(dir $@)
	$(CC) -m32 -I. -x assembler-with-cpp -c $< -o $@

$(PC_UNIFIED_BUILD)/stage0.elf: $(PC_UNIFIED_BUILD)/stage0.o \
	$(PC_UNIFIED)/stage0.ld
	$(LD) -m elf_i386 -T $(PC_UNIFIED)/stage0.ld $< -o $@

$(PC_UNIFIED_BUILD)/stage0.bin: $(PC_UNIFIED_BUILD)/stage0.elf
	$(OBJCOPY) -O binary -j .text $< $@
	@test $$(stat -c%s $@) -eq 512
	@test "$$(od -An -tx1 -j2 -N6 $@ | tr -d ' \n')" = 909049504c31
	@test "$$(od -An -tx1 -j510 -N2 $@ | tr -d ' \n')" = 55aa

$(PC_UNIFIED_BUILD)/pcat-stage1.o: $(PC_UNIFIED)/pcat-stage1.S \
	$(PC_UNIFIED_COMMON_DEPS)
	@mkdir -p $(dir $@)
	$(CC) -m32 -I. -x assembler-with-cpp -c $< -o $@

$(PC_UNIFIED_BUILD)/pc98-stage1.o: $(PC_UNIFIED)/pc98-stage1.S \
	$(PC_UNIFIED_COMMON_DEPS)
	@mkdir -p $(dir $@)
	$(CC) -m32 -I. -x assembler-with-cpp -c $< -o $@

$(PC_UNIFIED_BUILD)/%-stage1.elf: $(PC_UNIFIED_BUILD)/%-stage1.o \
	$(PC_UNIFIED)/stage1.ld
	$(LD) -m elf_i386 -T $(PC_UNIFIED)/stage1.ld $< -o $@

$(PC_UNIFIED_BUILD)/%-stage1.bin: $(PC_UNIFIED_BUILD)/%-stage1.elf
	$(OBJCOPY) -O binary -j .text $< $@
	@test $$(stat -c%s $@) -eq 512

$(PC_UNIFIED_BUILD)/pcat-stage2.o: $(PC_UNIFIED)/pcat-stage2.S \
	bootloader/pcat/stage2.S bootloader/include/disk-layout.inc \
	bootloader/include/stage2-header.inc bootloader/include/mbr.inc \
	bootloader/include/fat16.inc bootloader/include/elf.inc \
	bootloader/include/amd64-handoff.h
	@mkdir -p $(dir $@)
	$(CC) -m64 -I. -x assembler-with-cpp -c $< -o $@

$(PC_UNIFIED_BUILD)/pc98-stage2.o: $(PC_UNIFIED)/pc98-stage2.S \
	bootloader/pc98/stage2.S bootloader/include/disk-layout.inc \
	bootloader/include/stage2-header.inc bootloader/include/mbr.inc \
	bootloader/include/fat16.inc bootloader/include/elf.inc
	@mkdir -p $(dir $@)
	$(CC) -m64 -I. -x assembler-with-cpp -c $< -o $@

$(PC_UNIFIED_BUILD)/pcat-stage2.elf: $(PC_UNIFIED_BUILD)/pcat-stage2.o \
	bootloader/pcat/stage2.ld
	$(LD) -m elf_x86_64 -T bootloader/pcat/stage2.ld $< -o $@

$(PC_UNIFIED_BUILD)/pc98-stage2.elf: $(PC_UNIFIED_BUILD)/pc98-stage2.o \
	bootloader/pc98/stage2.ld
	$(LD) -m elf_x86_64 -T bootloader/pc98/stage2.ld $< -o $@

$(PC_UNIFIED_BUILD)/%-stage2.raw: $(PC_UNIFIED_BUILD)/%-stage2.elf
	$(OBJCOPY) -O binary -j .text $< $@

$(PC_UNIFIED_BUILD)/pcat-stage2.bin: $(PC_UNIFIED_BUILD)/pcat-stage2.raw \
	$(SCRIPTS_DIR)/finalize-bios-stage2.py
	$(PYTHON) $(SCRIPTS_DIR)/finalize-bios-stage2.py --machine pcat $< $@
	@test $$(($$(stat -c%s $@) / 512)) -lt 64

$(PC_UNIFIED_BUILD)/pc98-stage2.bin: $(PC_UNIFIED_BUILD)/pc98-stage2.raw \
	$(SCRIPTS_DIR)/finalize-bios-stage2.py
	$(PYTHON) $(SCRIPTS_DIR)/finalize-bios-stage2.py --machine pc98 $< $@
	@test $$(($$(stat -c%s $@) / 512)) -lt 64

pc-unified-bootloader: $(PC_UNIFIED_BUILD)/stage0.bin \
	$(PC_UNIFIED_BUILD)/pcat-stage1.bin \
	$(PC_UNIFIED_BUILD)/pc98-stage1.bin \
	$(PC_UNIFIED_BUILD)/pcat-stage2.bin \
	$(PC_UNIFIED_BUILD)/pc98-stage2.bin

pc-unified-kernels:
	$(MAKE) -C $(CURDIR) ARCH=pcat messages
	$(MAKE) -C $(CURDIR) ARCH=pcat build/pcat/vmunix build/pcat/bin/sh \
		build/pcat/bin/noct
	$(MAKE) -C $(CURDIR) ARCH=pc98 messages
	$(MAKE) -C $(CURDIR) ARCH=pc98 build/pc98/vmunix
	$(MAKE) -C $(CURDIR) ARCH=amd64-pcat messages
	$(MAKE) -C $(CURDIR) ARCH=amd64-pcat build/amd64-pcat/vmunix
	@mkdir -p $(PC_UNIFIED_BUILD)
	cp -f build/pcat/vmunix $(PC_UNIFIED_BUILD)/vmunix.at
	cp -f build/pc98/vmunix $(PC_UNIFIED_BUILD)/vmunix.98
	cp -f build/amd64-pcat/vmunix $(PC_UNIFIED_BUILD)/vmunix.x64
	cp -f build/pcat/bin/sh $(PC_UNIFIED_BUILD)/sh
	cp -f build/pcat/bin/noct $(PC_UNIFIED_BUILD)/noct

$(PC_UNIFIED_BUILD)/hdd-image.img: pc-unified-bootloader \
	pc-unified-kernels $(UEFI_BUILD)/BOOTX64.EFI $(HOLORIS_NOCT) \
	$(SCRIPTS_DIR)/make-pc-unified-hdd-image.py \
	$(SCRIPTS_DIR)/check-pc-unified-hdd-image.py
	$(PYTHON) $(SCRIPTS_DIR)/make-pc-unified-hdd-image.py --force \
		--stage0 $(PC_UNIFIED_BUILD)/stage0.bin \
		--pc98-stage1 $(PC_UNIFIED_BUILD)/pc98-stage1.bin \
		--pc98-stage2 $(PC_UNIFIED_BUILD)/pc98-stage2.bin \
		--pcat-stage1 $(PC_UNIFIED_BUILD)/pcat-stage1.bin \
		--pcat-stage2 $(PC_UNIFIED_BUILD)/pcat-stage2.bin \
		--pc98-kernel $(PC_UNIFIED_BUILD)/vmunix.98 \
		--pcat-kernel $(PC_UNIFIED_BUILD)/vmunix.at \
		--amd64-kernel $(PC_UNIFIED_BUILD)/vmunix.x64 \
		--bootx64 $(UEFI_BUILD)/BOOTX64.EFI \
		--shell $(PC_UNIFIED_BUILD)/sh \
		--noct $(PC_UNIFIED_BUILD)/noct \
		--holoris $(HOLORIS_NOCT) $@

pc-unified-hdd-image: $(PC_UNIFIED_BUILD)/hdd-image.img

pc-unified-loader-host-check: $(PC_UNIFIED_BUILD)/hdd-image.img
	$(PYTHON) $(SCRIPTS_DIR)/check-pc-unified-hdd-image.py \
		--pc98-kernel $(PC_UNIFIED_BUILD)/vmunix.98 \
		--pcat-kernel $(PC_UNIFIED_BUILD)/vmunix.at \
		--amd64-kernel $(PC_UNIFIED_BUILD)/vmunix.x64 \
		--bootx64 $(UEFI_BUILD)/BOOTX64.EFI \
		--noct $(PC_UNIFIED_BUILD)/noct \
		--holoris $(HOLORIS_NOCT) $<

pc-unified-loader-qemu-test: pc-unified-bootloader \
	$(UEFI_BUILD)/BOOTX64.EFI
	$(MAKE) -C $(CURDIR) ARCH=pcat build/pcat/bootloader/payload32.elf \
		build/pcat/bootloader/payload64.elf
	$(MAKE) -C $(CURDIR) ARCH=pc98 build/pc98/bootloader/payload32.elf
	bash $(SCRIPTS_DIR)/test-pc-unified-bootloader-qemu.sh

uefi-entry-qemu-test amd64-uefi-qemu-test: \
	$(PC_UNIFIED_BUILD)/hdd-image.img
	bash $(SCRIPTS_DIR)/test-amd64-pcat-uefi-qemu.sh $<

.PHONY: uefi-loader uefi-loader-host-check pc-unified-bootloader \
	pc-unified-kernels pc-unified-hdd-image pc-unified-loader-host-check \
	pc-unified-loader-qemu-test uefi-entry-qemu-test amd64-uefi-qemu-test

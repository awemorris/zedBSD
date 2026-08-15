# Unified PC/AT BIOS, PC-98 BIOS, and x64 UEFI image rules.
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib

UNIFIED := bootloader/unified
UNIFIED_BUILD := build/unified
UEFI_LOADER := bootloader/uefi
UEFI_BUILD := build/uefi
EFI_CC ?= x86_64-w64-mingw32-gcc
EFI_LD ?= x86_64-w64-mingw32-ld
REMACS_NAP := $(UNIFIED_BUILD)/remacs.nap
REMACS_ROOT := $(NOCT_ROOT)/apps/remacs
REMACS_SOURCES := $(wildcard $(REMACS_ROOT)/editor/*.noct) \
	$(REMACS_ROOT)/src/napi.def $(REMACS_ROOT)/tools/gen-napi.py \
	$(REMACS_ROOT)/tools/build-nap.sh
EFI_NM ?= x86_64-w64-mingw32-nm
EFI_CFLAGS := -std=c11 -ffreestanding -fshort-wchar -mno-red-zone \
	-fno-stack-protector -fno-builtin -fno-asynchronous-unwind-tables \
	-fno-unwind-tables -fno-ident -ffunction-sections -fdata-sections \
	-Os -Wall -Wextra -Werror -I.
UNIFIED_COMMON_DEPS := $(UNIFIED)/layout.inc \
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

$(UNIFIED_BUILD)/stage0.o: $(UNIFIED)/stage0.S \
	$(UNIFIED)/layout.inc
	@mkdir -p $(dir $@)
	$(CC) -m32 -I. -x assembler-with-cpp -c $< -o $@

$(UNIFIED_BUILD)/stage0.elf: $(UNIFIED_BUILD)/stage0.o \
	$(UNIFIED)/stage0.ld
	$(LD) -m elf_i386 -T $(UNIFIED)/stage0.ld $< -o $@

$(UNIFIED_BUILD)/stage0.bin: $(UNIFIED_BUILD)/stage0.elf
	$(OBJCOPY) -O binary -j .text $< $@
	@test $$(stat -c%s $@) -eq 512
	@test "$$(od -An -tx1 -j2 -N6 $@ | tr -d ' \n')" = 909049504c31
	@test "$$(od -An -tx1 -j510 -N2 $@ | tr -d ' \n')" = 55aa

$(UNIFIED_BUILD)/pcat-stage1.o: $(UNIFIED)/pcat-stage1.S \
	$(UNIFIED_COMMON_DEPS)
	@mkdir -p $(dir $@)
	$(CC) -m32 -I. -x assembler-with-cpp -c $< -o $@

$(UNIFIED_BUILD)/pc98-stage1.o: $(UNIFIED)/pc98-stage1.S \
	$(UNIFIED_COMMON_DEPS)
	@mkdir -p $(dir $@)
	$(CC) -m32 -I. -x assembler-with-cpp -c $< -o $@

$(UNIFIED_BUILD)/%-stage1.elf: $(UNIFIED_BUILD)/%-stage1.o \
	$(UNIFIED)/stage1.ld
	$(LD) -m elf_i386 -T $(UNIFIED)/stage1.ld $< -o $@

$(UNIFIED_BUILD)/%-stage1.bin: $(UNIFIED_BUILD)/%-stage1.elf
	$(OBJCOPY) -O binary -j .text $< $@
	@test $$(stat -c%s $@) -eq 512

$(UNIFIED_BUILD)/pcat-stage2.o: $(UNIFIED)/pcat-stage2.S \
	bootloader/pcat/stage2.S bootloader/include/disk-layout.inc \
	bootloader/include/stage2-header.inc bootloader/include/mbr.inc \
	bootloader/include/fat16.inc bootloader/include/elf.inc \
	bootloader/include/amd64-handoff.h
	@mkdir -p $(dir $@)
	$(CC) -m64 -I. -x assembler-with-cpp -c $< -o $@

$(UNIFIED_BUILD)/pc98-stage2.o: $(UNIFIED)/pc98-stage2.S \
	bootloader/pc98/stage2.S bootloader/include/disk-layout.inc \
	bootloader/include/stage2-header.inc bootloader/include/mbr.inc \
	bootloader/include/fat16.inc bootloader/include/elf.inc
	@mkdir -p $(dir $@)
	$(CC) -m64 -I. -x assembler-with-cpp -c $< -o $@

$(UNIFIED_BUILD)/pcat-stage2.elf: $(UNIFIED_BUILD)/pcat-stage2.o \
	bootloader/pcat/stage2.ld
	$(LD) -m elf_x86_64 -T bootloader/pcat/stage2.ld $< -o $@

$(UNIFIED_BUILD)/pc98-stage2.elf: $(UNIFIED_BUILD)/pc98-stage2.o \
	bootloader/pc98/stage2.ld
	$(LD) -m elf_x86_64 -T bootloader/pc98/stage2.ld $< -o $@

$(UNIFIED_BUILD)/%-stage2.raw: $(UNIFIED_BUILD)/%-stage2.elf
	$(OBJCOPY) -O binary -j .text $< $@

$(UNIFIED_BUILD)/pcat-stage2.bin: $(UNIFIED_BUILD)/pcat-stage2.raw \
	$(SCRIPTS_DIR)/finalize-bios-stage2.py
	$(PYTHON) $(SCRIPTS_DIR)/finalize-bios-stage2.py --machine pcat $< $@
	@test $$(($$(stat -c%s $@) / 512)) -lt 64

$(UNIFIED_BUILD)/pc98-stage2.bin: $(UNIFIED_BUILD)/pc98-stage2.raw \
	$(SCRIPTS_DIR)/finalize-bios-stage2.py
	$(PYTHON) $(SCRIPTS_DIR)/finalize-bios-stage2.py --machine pc98 $< $@
	@test $$(($$(stat -c%s $@) / 512)) -lt 64

unified-bootloader: $(UNIFIED_BUILD)/stage0.bin \
	$(UNIFIED_BUILD)/pcat-stage1.bin \
	$(UNIFIED_BUILD)/pc98-stage1.bin \
	$(UNIFIED_BUILD)/pcat-stage2.bin \
	$(UNIFIED_BUILD)/pc98-stage2.bin

unified-kernels:
	$(MAKE) -C $(CURDIR) ARCH=pcat messages
	$(MAKE) -C $(CURDIR) ARCH=pcat build/pcat/vmunix arch-image
	$(MAKE) -C $(CURDIR) ARCH=pc98 messages
	$(MAKE) -C $(CURDIR) ARCH=pc98 build/pc98/vmunix
	$(MAKE) -C $(CURDIR) ARCH=amd64 messages
	$(MAKE) -C $(CURDIR) ARCH=amd64 build/amd64/vmunix arch-image
	$(MAKE) -C $(CURDIR) ARCH=arm64 messages
	$(MAKE) -C $(CURDIR) ARCH=arm64 build/arm64/VMUNIX.A64 arch-image
	@mkdir -p $(UNIFIED_BUILD)
	cp -f build/pcat/vmunix $(UNIFIED_BUILD)/vmunix.at
	cp -f build/pc98/vmunix $(UNIFIED_BUILD)/vmunix.98
	cp -f build/amd64/vmunix $(UNIFIED_BUILD)/vmunix.x64
	cp -f build/arm64/VMUNIX.A64 $(UNIFIED_BUILD)/VMUNIX.A64

$(REMACS_NAP): $(SCRIPTS_DIR)/build-remacs-nap.sh $(REMACS_SOURCES)
	bash $(SCRIPTS_DIR)/build-remacs-nap.sh $@

$(UNIFIED_BUILD)/hdd-image.img: unified-bootloader \
	unified-kernels $(UEFI_BUILD)/BOOTX64.EFI $(HOLORIS_NOCT) $(REMACS_NAP) \
	$(SCRIPTS_DIR)/make-unified-hdd-image.py \
	$(SCRIPTS_DIR)/check-unified-hdd-image.py
	$(PYTHON) $(SCRIPTS_DIR)/make-unified-hdd-image.py --force \
		--stage0 $(UNIFIED_BUILD)/stage0.bin \
		--pc98-stage1 $(UNIFIED_BUILD)/pc98-stage1.bin \
		--pc98-stage2 $(UNIFIED_BUILD)/pc98-stage2.bin \
		--pcat-stage1 $(UNIFIED_BUILD)/pcat-stage1.bin \
		--pcat-stage2 $(UNIFIED_BUILD)/pcat-stage2.bin \
		--pc98-kernel $(UNIFIED_BUILD)/vmunix.98 \
		--pcat-kernel $(UNIFIED_BUILD)/vmunix.at \
		--amd64-kernel $(UNIFIED_BUILD)/vmunix.x64 \
		--arm64-kernel $(UNIFIED_BUILD)/VMUNIX.A64 \
		--i386-arch-image $(ARCH_IMAGE_DIR)/i386.img \
		--amd64-arch-image $(ARCH_IMAGE_DIR)/amd64.img \
		--aarch64-arch-image $(ARCH_IMAGE_DIR)/aarch64.img \
		--rpi4-config platform/arm64/config.txt \
		--rpi4-firmware-dir vendor/raspberrypi-firmware/boot \
		--bootx64 $(UEFI_BUILD)/BOOTX64.EFI \
		--holoris $(HOLORIS_NOCT) \
		--remacs $(REMACS_NAP) $@

unified-hdd-image: $(UNIFIED_BUILD)/hdd-image.img

unified-loader-host-check: $(UNIFIED_BUILD)/hdd-image.img
	$(PYTHON) $(SCRIPTS_DIR)/check-unified-hdd-image.py \
		--pc98-kernel $(UNIFIED_BUILD)/vmunix.98 \
		--pcat-kernel $(UNIFIED_BUILD)/vmunix.at \
		--amd64-kernel $(UNIFIED_BUILD)/vmunix.x64 \
		--arm64-kernel $(UNIFIED_BUILD)/VMUNIX.A64 \
		--i386-arch-image $(ARCH_IMAGE_DIR)/i386.img \
		--amd64-arch-image $(ARCH_IMAGE_DIR)/amd64.img \
		--aarch64-arch-image $(ARCH_IMAGE_DIR)/aarch64.img \
		--rpi4-config platform/arm64/config.txt \
		--rpi4-firmware-dir vendor/raspberrypi-firmware/boot \
		--bootx64 $(UEFI_BUILD)/BOOTX64.EFI \
		--holoris $(HOLORIS_NOCT) \
		--remacs $(REMACS_NAP) $<

unified-loader-qemu-test: unified-bootloader \
	$(UEFI_BUILD)/BOOTX64.EFI
	$(MAKE) -C $(CURDIR) ARCH=pcat build/pcat/bootloader/payload32.elf \
		build/pcat/bootloader/payload64.elf
	$(MAKE) -C $(CURDIR) ARCH=pc98 build/pc98/bootloader/payload32.elf
	bash $(SCRIPTS_DIR)/test-unified-bootloader-qemu.sh

uefi-entry-qemu-test amd64-uefi-qemu-test: \
	$(UNIFIED_BUILD)/hdd-image.img
	bash $(SCRIPTS_DIR)/test-amd64-uefi-qemu.sh $<

.PHONY: uefi-loader uefi-loader-host-check unified-bootloader \
	unified-kernels unified-hdd-image unified-loader-host-check \
	unified-loader-qemu-test uefi-entry-qemu-test amd64-uefi-qemu-test

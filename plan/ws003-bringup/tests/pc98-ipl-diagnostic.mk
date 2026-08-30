# WS003 p023 PC-9821V13 diagnostic-only IPL image.
# Copyright (C) 2026 Awe Morris
# SPDX-License-Identifier: Zlib

WS003_P023_BUILD := $(BUILD)/ws003-p023
WS003_P023_STAGE1_OBJ := $(WS003_P023_BUILD)/bootloader/stage1.o
WS003_P023_STAGE1_ELF := $(WS003_P023_BUILD)/bootloader/stage1.elf
WS003_P023_STAGE1_BIN := $(WS003_P023_BUILD)/bootloader/stage1.bin
WS003_P023_STAGE2_OBJ := $(WS003_P023_BUILD)/bootloader/stage2.o
WS003_P023_STAGE2_ELF := $(WS003_P023_BUILD)/bootloader/stage2.elf
WS003_P023_STAGE2_BIN := $(WS003_P023_BUILD)/bootloader/stage2.bin
WS003_P023_IMAGE := $(BUILD)/ws003-p023-pc9821-v13-diagnostic.img
WS003_P023_HASH := $(WS003_P023_IMAGE).sha256

ifneq ($(ZEDBSD_PLATFORM_DIR),pc98)
$(error ws003-p023 diagnostic image requires the PC-98 target)
endif

$(WS003_P023_STAGE1_OBJ): bootloader/pc98/disk-ipl.S
	@mkdir -p $(dir $@)
	$(CC) -m32 -I. -DZEDBSD_PC98_IPL_DIAGNOSTIC=1 \
		-x assembler-with-cpp -c $< -o $@

$(WS003_P023_STAGE1_ELF): $(WS003_P023_STAGE1_OBJ) \
	bootloader/pc98/stage1.ld
	$(LD) -m elf_i386 -T bootloader/pc98/stage1.ld $< -o $@

$(WS003_P023_STAGE1_BIN): $(WS003_P023_STAGE1_ELF)
	$(OBJCOPY) -O binary -j .text $< $@
	@test $$(stat -c%s $@) -eq 512
	@test "$$(od -An -tc -j4 -N4 $@ | tr -d ' \n')" = IPL1
	@test "$$(od -An -tx1 -j510 -N2 $@ | tr -d ' \n')" = 55aa

$(WS003_P023_STAGE2_OBJ): bootloader/pc98/lba2.S
	@mkdir -p $(dir $@)
	$(CC) -m32 -I. -DZEDBSD_PC98_IPL_DIAGNOSTIC=1 \
		-x assembler-with-cpp -c $< -o $@

$(WS003_P023_STAGE2_ELF): $(WS003_P023_STAGE2_OBJ)
	$(LD) -m elf_i386 -Ttext=0 -e _start $< -o $@

$(WS003_P023_STAGE2_BIN): $(WS003_P023_STAGE2_ELF)
	$(OBJCOPY) -O binary -j .text $< $@
	@test $$(stat -c%s $@) -eq 7168
	@test "$$(od -An -tx1 -j510 -N2 $@ | tr -d ' \n')" = 55aa

.DELETE_ON_ERROR: $(WS003_P023_IMAGE) $(WS003_P023_HASH)

$(WS003_P023_IMAGE): $(DISK_IMAGE_ARTIFACT) \
	$(WS003_P023_STAGE1_BIN) $(WS003_P023_STAGE2_BIN)
	@mkdir -p $(dir $@)
	cp --reflink=auto --sparse=always $(DISK_IMAGE_ARTIFACT) $@.tmp
	dd if=$(WS003_P023_STAGE1_BIN) of=$@.tmp bs=512 seek=0 count=1 \
		conv=notrunc status=none
	dd if=$(WS003_P023_STAGE2_BIN) of=$@.tmp bs=512 seek=2 count=14 \
		conv=notrunc status=none
	mv -f $@.tmp $@

$(WS003_P023_HASH): $(WS003_P023_IMAGE)
	sha256sum $< >$@.tmp
	mv -f $@.tmp $@

.PHONY: ws003-p023-diagnostic-image
ws003-p023-diagnostic-image: $(WS003_P023_IMAGE) $(WS003_P023_HASH)
	@printf 'WS003 p023 diagnostic image: %s\n' \
		'$(abspath $(WS003_P023_IMAGE))'
	@cat $(WS003_P023_HASH)

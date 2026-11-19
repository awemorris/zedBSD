# zedBSD
# Copyright (C) 2026 Awe Morris
#
# SPDX-License-Identifier: Zlib

#
# 68040 Macintosh boot loader rules
#
# The ROM reads the boot block (the first 1 KiB of the HFS boot
# partition) through the disk's own driver and runs it.  The boot block
# reads the second stage, the file zedboot of the same partition, and
# enters it; the second stage reads the kernel through the ROM's File
# Manager, builds the boot information and enters the kernel
# (bootloader/mac68k/).  The disk image's tool (image.mk) puts both
# in the partition and records in the boot block where they are.
#
# The second stage runs wherever the ROM's heap puts it: it is linked at
# 0 with its relocations kept, and finalize-mac68k-stage2.py appends the
# list of the 32-bit addresses its entry adds its own address to.
#

MAC68K_BOOT_DIR   := bootloader/mac68k
MAC68K_BOOT_BUILD := $(BUILD)/bootloader

MAC68K_BOOT_CPPFLAGS := \
	-nostdinc \
	-Iinclude/libc \
	-Iinclude \
	-I. \
	-D__ZEDBSD__

# The ROM's globals live in the first page of memory, which GCC would
# otherwise take for a null pointer's (min-pagesize).
MAC68K_BOOT_CFLAGS := \
	-m68040 \
	-msoft-float \
	-ffreestanding \
	-fno-pic \
	-fno-pie \
	-fno-common \
	-fno-strict-aliasing \
	-fno-stack-protector \
	-fno-asynchronous-unwind-tables \
	-fno-unwind-tables \
	-fno-builtin \
	--param=min-pagesize=0 \
	-Os \
	-Wall \
	-Wextra \
	-Werror \
	-MMD \
	-MP

# The loader's objects, from assembly and from C.
MAC68K_BOOT_ASM_OBJS := \
	$(MAC68K_BOOT_BUILD)/stage1.o \
	$(MAC68K_BOOT_BUILD)/stage2-start.o \
	$(MAC68K_BOOT_BUILD)/toolbox.o \
	$(MAC68K_BOOT_BUILD)/cpu.o

MAC68K_BOOT_C_OBJS := \
	$(MAC68K_BOOT_BUILD)/stage2.o \
	$(MAC68K_BOOT_BUILD)/screen.o \
	$(MAC68K_BOOT_BUILD)/font.o \
	$(MAC68K_BOOT_BUILD)/memory.o \
	$(MAC68K_BOOT_BUILD)/elf.o \
	$(MAC68K_BOOT_BUILD)/bootinfo.o \
	$(MAC68K_BOOT_BUILD)/config.o \
	$(MAC68K_BOOT_BUILD)/handoff.o \
	$(MAC68K_BOOT_BUILD)/lib.o \
	$(MAC68K_BOOT_BUILD)/string.o

MAC68K_BOOT_OBJS := $(MAC68K_BOOT_ASM_OBJS) $(MAC68K_BOOT_C_OBJS)

# The boot block's object, and the second stage's (its entry first).
MAC68K_STAGE1_OBJS := $(MAC68K_BOOT_BUILD)/stage1.o
MAC68K_STAGE2_OBJS := $(filter-out $(MAC68K_STAGE1_OBJS),$(MAC68K_BOOT_OBJS))

MAC68K_BOOT_BLOCK := $(MAC68K_BOOT_BUILD)/bootblock.bin
MAC68K_STAGE2     := $(MAC68K_BOOT_BUILD)/zedboot

.PHONY: bootloader-platform
bootloader-platform: $(MAC68K_BOOT_BLOCK) $(MAC68K_STAGE2)

# The boot block: position independent, at most 1 KiB, no relocation.
$(MAC68K_BOOT_BLOCK): $(MAC68K_STAGE1_OBJS) $(MAC68K_BOOT_DIR)/stage1.ld
	@echo "  LD      bootblock.bin"
	@$(LD) -T $(MAC68K_BOOT_DIR)/stage1.ld -nostdlib -o $(MAC68K_BOOT_BUILD)/stage1.elf $(MAC68K_STAGE1_OBJS)
	@test -z "$$($(READELF) -r $(MAC68K_BOOT_BUILD)/stage1.elf | grep R_68K)" || { echo "the boot block has relocations"; exit 1; }
	@$(OBJCOPY) -O binary $(MAC68K_BOOT_BUILD)/stage1.elf $@

# The second stage: linked at 0 with its relocations, then finished with
# the list of its addresses.
$(MAC68K_STAGE2): $(MAC68K_STAGE2_OBJS) $(MAC68K_BOOT_DIR)/stage2.ld mk/tools/finalize-mac68k-stage2.py
	@echo "  LD      zedboot"
	@$(LD) -T $(MAC68K_BOOT_DIR)/stage2.ld -nostdlib --emit-relocs --no-warn-rwx-segments -Map $(MAC68K_BOOT_BUILD)/zedboot.map -o $(MAC68K_BOOT_BUILD)/zedboot.elf $(MAC68K_STAGE2_OBJS) $(MAC68K_LIBGCC)
	@test -z "$$($(NM) -u $(MAC68K_BOOT_BUILD)/zedboot.elf)" || { $(NM) -u $(MAC68K_BOOT_BUILD)/zedboot.elf; exit 1; }
	@$(PYTHON) mk/tools/finalize-mac68k-stage2.py --input $(MAC68K_BOOT_BUILD)/zedboot.elf --output $@

$(MAC68K_BOOT_ASM_OBJS): $(MAC68K_BOOT_BUILD)/%.o: $(MAC68K_BOOT_DIR)/%.S $(MAC68K_BOOT_BUILD)/.flags
	@echo "  AS      $<"
	@mkdir -p $(dir $@)
	@$(CC) $(MAC68K_BOOT_CPPFLAGS) $(MAC68K_BOOT_CFLAGS) -c $< -o $@

$(MAC68K_BOOT_C_OBJS): $(MAC68K_BOOT_BUILD)/%.o: $(MAC68K_BOOT_DIR)/%.c $(MAC68K_BOOT_BUILD)/.flags
	@echo "  CC      $<"
	@mkdir -p $(dir $@)
	@$(CC) $(MAC68K_BOOT_CPPFLAGS) $(MAC68K_BOOT_CFLAGS) -c $< -o $@

# The loader's objects follow its compile flags, as the kernel's do: the
# stamp is written every time and replaced only when the flags change.
.PHONY: FORCE_MAC68K_BOOT_FLAGS
FORCE_MAC68K_BOOT_FLAGS:
$(MAC68K_BOOT_BUILD)/.flags: FORCE_MAC68K_BOOT_FLAGS
	@mkdir -p $(dir $@)
	@printf '%s\n' '$(CC) $(MAC68K_BOOT_CPPFLAGS) $(MAC68K_BOOT_CFLAGS)' > $@.tmp
	@if cmp -s $@.tmp $@; then rm -f $@.tmp; else mv -f $@.tmp $@; fi

# The loader's objects also follow the headers they include.
-include $(MAC68K_BOOT_OBJS:.o=.d)
